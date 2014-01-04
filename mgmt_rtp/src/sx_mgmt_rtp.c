#include "stdio.h" 
#include "stdlib.h" 
#include "string.h" 
#include <sys/socket.h>
#include <netinet/in.h> 
#include <netdb.h> 
#include "pthread.h"
#include "fcntl.h"
#include "mqueue.h"
#include "assert.h"

#include "sx_mgmt_rtp.h"
#include "sx_mgmt_video.h"
#include "nal_to_rtp.h"

#define MGMT_RTP_MSG_QUEUE  "/mgmt_rtp_msg_queue"

typedef enum
{
    MGMT_RTP_STATE_IDLE,
    MGMT_RTP_STATE_ACTIVE,

} eMGMT_RTP_STATE;


typedef enum
{
   MGMT_RTP_EVENT_ACTIVATE,
   MGMT_RTP_EVENT_RESET,
   MGMT_RTP_EVENT_SERVICE,

} eMGMT_RTP_EVENT;


typedef struct
{
    unsigned int    id; 
    unsigned int    ip;
    unsigned short  port;

} sMGMT_RTP_EVENT_DATA_ACTIVATE;


typedef struct
{
    unsigned int    id; 

} sMGMT_RTP_EVENT_DATA_RESET;


typedef union
{
    sMGMT_RTP_EVENT_DATA_ACTIVATE   activate;
    sMGMT_RTP_EVENT_DATA_RESET      reset; 

} uMGMT_RTP_EVENT_DATA;


typedef struct
{
    eMGMT_RTP_EVENT         event;
    uMGMT_RTP_EVENT_DATA    event_data;

} sMGMT_RTP_MSG;


typedef struct
{
    unsigned char       in_use; 
    struct sockaddr_in  peer_addr;
    unsigned char       sps_sent; 
    unsigned char       pps_sent; 
    unsigned char       idr_observed; 

    void               *nal_to_rtp_instance;

} sSESSION; 


// RTP manager control block.
typedef struct
{
    pthread_t           rtp_thread;
    pthread_t           timer_thread;
    int                 rtp_sock;
    mqd_t               msg_queue;
    unsigned int        ip;
    unsigned short      port;
    struct sockaddr_in  peer_addr;
    eMGMT_RTP_STATE     state;
    sSESSION            sessions[32]; 

} sMGMT_RTP_CBLK;


// Control block. 
static sMGMT_RTP_CBLK f_cblk;


//  Sends NAL unit as RTP packets.
static void rtp_send(
    int             sock, 
    sSESSION       *session, 
    unsigned char  *nal_unit, 
    unsigned int    nal_unit_len
    )
{
    sRTP_PKT_NODE  *head;
    sRTP_PKT_NODE  *temp;


    // Get the converted RTP chain.
    head = sx_nal_to_rtp_util_get(session->nal_to_rtp_instance,
                                  nal_unit,
                                  nal_unit_len);
    temp = head; 

    logger_log("Peer IP: 0x%x", session->peer_addr);

    do
    {
        // Send the packet.
        int rv = sendto(sock,
                        &temp->rtp_pkt,
                        temp->rtp_pkt_len,
                        0,
                        (struct sockaddr *) &session->peer_addr,
                        sizeof(struct sockaddr));
        assert(rv == temp->rtp_pkt_len);

        temp = temp->next; 

    } while (temp); 

    // Free the NAL unit.
    sx_nal_to_rtp_util_free(head); 
}


static void idle_state_handler(
    sMGMT_RTP_MSG  *msg
    )
{
    
}


static void activate_handler(
    sMGMT_RTP_MSG  *msg
    )
{
    sSESSION *session;


    logger_log("MGMT_RTP: ACTIVATE received [id = %d]",
               msg->event_data.activate.id);


    session = &f_cblk.sessions[msg->event_data.activate.id];

    session->in_use                     = 1;
    session->peer_addr.sin_family       = AF_INET;
    session->peer_addr.sin_addr.s_addr  = msg->event_data.activate.ip;
    session->peer_addr.sin_port         = htons(msg->event_data.activate.port);
    bzero(&(session->peer_addr.sin_zero), 8);

    session->idr_observed   = 0;
    session->pps_sent       = 0;
    session->sps_sent       = 0;

    session->nal_to_rtp_instance = sx_nal_to_rtp_util_create();

    f_cblk.state = MGMT_RTP_STATE_ACTIVE;
}


static void reset_handler(
    sMGMT_RTP_MSG  *msg
    )
{
    logger_log("MGMT_RTP: RESET received [id = %d]",
            msg->event_data.reset.id);

    sSESSION *session = &f_cblk.sessions[msg->event_data.reset.id];

    session->in_use = 0;

    sx_nal_to_rtp_util_destroy(session->nal_to_rtp_instance);

    session->nal_to_rtp_instance = NULL;
}


static void service_handler(
    sMGMT_RTP_MSG  *msg
    )
{
    unsigned int            i;
    sMGMT_VIDEO_NAL_UNIT   *nal_unit;
    sMGMT_VIDEO_NAL_UNIT   *sps_nal_unit;
    sMGMT_VIDEO_NAL_UNIT   *pps_nal_unit;
    sMGMT_VIDEO_NAL_UNIT   *nal_unit_to_send;


    nal_unit = sx_mgmt_video_get_nal_unit();

    for(i = 0; i < 32; i++)
    {
        sSESSION *session = &f_cblk.sessions[i];

        if(session->in_use)
        {
            while(1)
            {
                if(!session->sps_sent)
                {
                    logger_log("Get SPS unit");

                    // Send SPS second.
                    sps_nal_unit = sx_mgmt_video_sps_get();
                    if(sps_nal_unit == NULL)
                    {
                        logger_log("mgmt_video_get_sps_nal_unit() returned NULL!");
                        break;
                    }

                    logger_log("SPS unit len %d", sps_nal_unit->nal_unit_len);

                    nal_unit_to_send = sps_nal_unit;

                    session->sps_sent = 1;
                }
                else if(!session->pps_sent)
                {
                    logger_log("Get PPS unit");

                    // Send PPS first.
                    pps_nal_unit = sx_mgmt_video_pps_get();
                    if(pps_nal_unit == NULL)
                    {
                        logger_log("mgmt_video_get_pps_nal_unit() returned NULL!");
                        break;
                    }

                    nal_unit_to_send = pps_nal_unit;

                    session->pps_sent = 1;
                }
                else
                {
                    // Send NAL units.

                    if(nal_unit == NULL)
                    {
                        // Out of NAL units. Done.
                        break;
                    }

                    if(!session->idr_observed)
                    {
                        // Start with key units.
                        if(!sx_mgmt_video_is_key_frame(nal_unit))
                        {
                            break;
                        }

                        session->idr_observed = 1;
                    }

                    nal_unit_to_send = nal_unit;
                }

                // Send RTP.
                rtp_send(f_cblk.rtp_sock,
                        session,
                        nal_unit_to_send->nal_unit,
                        nal_unit_to_send->nal_unit_len);

                break;
            }
        }
    }

    if(nal_unit != NULL)
    {
        // Free NAL unit.
        sx_mgmt_video_free_nal_unit(nal_unit);
    }
}


static void rtp_thread(
    void * arg
    )
{
    sMGMT_RTP_MSG           msg;
    unsigned int            bytes_read;



    while(1)
    {
        bytes_read = mq_receive(f_cblk.msg_queue,
                                (char *) &msg,
                                sizeof(msg),
                                NULL);
        assert(bytes_read > 0);

        switch(msg.event)
        {
            case MGMT_RTP_EVENT_ACTIVATE:
            {
                activate_handler(&msg);
                break;
            }
            case MGMT_RTP_EVENT_RESET:
            {
                reset_handler(&msg);
                break; 
            }
            case MGMT_RTP_EVENT_SERVICE:
            {
                service_handler(&msg);
                break; 
            }
            default:
            {
                assert(0);
            }
        }
    }
}


static void timer_thread(
    )
{
    sMGMT_RTP_MSG   msg;


    while (1)
    {
        // Construct message.
        msg.event = MGMT_RTP_EVENT_SERVICE;

        // Queue message.
        mq_send(f_cblk.msg_queue,
                (char *) &msg,
                sizeof(sMGMT_RTP_MSG),
                0);

        // 30 fps
        usleep(5*1000);
    }
}


static void rtp_thread_create(
    )
{
    pthread_create(&f_cblk.rtp_thread, NULL, (void *) &rtp_thread, NULL); 

    pthread_create(&f_cblk.timer_thread, NULL, (void *) &timer_thread, NULL);
}


void sx_mgmt_rtp_init(
    void
    )
{
    struct mq_attr queue_attr;


    // Set message attributes.
    queue_attr.mq_flags = 0;
    queue_attr.mq_maxmsg = 10;
    queue_attr.mq_msgsize = sizeof(sMGMT_RTP_MSG);
    queue_attr.mq_curmsgs = 0;

    // Create message queue.
    f_cblk.msg_queue = mq_open(MGMT_RTP_MSG_QUEUE,
                               O_CREAT | O_RDWR,
                               0644,
                               &queue_attr);

    f_cblk.rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); 
    assert(f_cblk.rtp_sock != 0); 
}


void sx_mgmt_rtp_open(
    void
    )
{
    // Create RTSP thread. 
    rtp_thread_create(); 
}


void sx_mgmt_rtp_activate(
    unsigned int    id, 
    unsigned int    ip, 
    unsigned short  port
    )
{
    sMGMT_RTP_MSG   msg;


    // Construct message.
    msg.event                       = MGMT_RTP_EVENT_ACTIVATE;
    msg.event_data.activate.id      = id;
    msg.event_data.activate.ip      = ip;
    msg.event_data.activate.port    = port;

    // Queue message.
    mq_send(f_cblk.msg_queue,
            (char *) &msg,
            sizeof(sMGMT_RTP_MSG),
            0);
}


void sx_mgmt_rtp_reset(
    unsigned int    id
    )
{
    sMGMT_RTP_MSG   msg;


    // Construct message.
    msg.event   = MGMT_RTP_EVENT_RESET;
    msg.event_data.reset.id = id; 

    // Queue message.
    mq_send(f_cblk.msg_queue,
            (char *) &msg,
            sizeof(sMGMT_RTP_MSG),
            0);
}
