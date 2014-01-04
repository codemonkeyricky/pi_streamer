#include "stdio.h"
#include "fcntl.h"
#include "mqueue.h"
#include "assert.h"

#include "sx_mgmt_rtsp.h"
#include "sx_mgmt_rtp.h"


#define MGMT_SYS_MSG_QUEUE  "/mgmt_sys_msg_queue"


typedef enum
{
   MGMT_SYS_EVENT_PLAY, 
   MGMT_SYS_EVENT_TEARDOWN, 

} eMGMT_SYS_EVENT;


typedef struct
{
    unsigned int    id; 

} sMGMT_SYS_EVENT_DATA_TEARDOWN;


typedef struct
{
    unsigned int    id; 
    unsigned int    ip;
    unsigned short  port;

} sMGMT_SYS_EVENT_DATA_PLAY;


typedef union
{
    sMGMT_SYS_EVENT_DATA_PLAY       play;
    sMGMT_SYS_EVENT_DATA_TEARDOWN   teardown; 

} uMGMT_SYS_EVENT_DATA;


typedef struct
{
    eMGMT_SYS_EVENT         event;
    uMGMT_SYS_EVENT_DATA    event_data;

} sMGMT_SYS_MSG;


typedef struct
{
    pthread_t       thread_id;
    mqd_t           msg_queue;
    unsigned int    active_session; 

} sMGMT_SYS_CBLK;

static sMGMT_SYS_CBLK   f_cblk;

static void rerouces_init(
    void
    )
{
    struct mq_attr queue_attr;


    // Set message attributes.
    queue_attr.mq_flags     = 0;
    queue_attr.mq_maxmsg    = 10;
    queue_attr.mq_msgsize   = sizeof(sMGMT_SYS_MSG);
    queue_attr.mq_curmsgs   = 0;

    // Create message queue.
    f_cblk.msg_queue = mq_open(MGMT_SYS_MSG_QUEUE,
                               O_CREAT | O_RDWR,
                               0644,
                               &queue_attr);
}


static void mgmt_rtsp_cback(
    void                   *arg,
    eSX_MGMT_RTSP_EVENT        event,
    uSX_MGMT_RTSP_EVENT_DATA  *event_data
    )
{
    sMGMT_SYS_MSG   msg;


    if(event == MGMT_RTSP_EVENT_PLAY)
    {
        msg.event = MGMT_SYS_EVENT_PLAY;
        msg.event_data.play.id = event_data->play.id;
        msg.event_data.play.ip = event_data->play.ip;
        msg.event_data.play.port = event_data->play.port;

        mq_send(f_cblk.msg_queue,
                (char *) &msg,
                sizeof(sMGMT_SYS_MSG),
                0);
    }

    if(event == MGMT_RTSP_EVENT_TEARDOWN)
    {
        msg.event = MGMT_SYS_EVENT_TEARDOWN;
        msg.event_data.teardown.id = event_data->teardown.id; 

        mq_send(f_cblk.msg_queue,
                (char *) &msg,
                sizeof(sMGMT_SYS_MSG),
                0);
    }
}


static void mgmt_sys_thread(
    void * arg
    )
{
    sMGMT_SYS_MSG   msg;
    unsigned int    bytes_read;

    // Open RTSP manager.
    sx_mgmt_rtsp_open();

    // Open RTP manager.
    sx_mgmt_rtp_open();

    // Open video manager. 
    sx_mgmt_video_open(); 

    while(1)
    {
        bytes_read = mq_receive(f_cblk.msg_queue,
                                (char *) &msg,
                                sizeof(sMGMT_SYS_MSG),
                                NULL);
        assert(bytes_read > 0);

        switch(msg.event)
        {
            case MGMT_SYS_EVENT_PLAY:
            {
                // Activate RTP manager.
                sx_mgmt_rtp_activate(msg.event_data.play.id,
                                     msg.event_data.play.ip,
                                     msg.event_data.play.port);

                if(f_cblk.active_session == 0)
                {
                    logger_log("mgmt_video_activate() Invoked");

                    sx_mgmt_video_activate(); 
                }

                f_cblk.active_session++; 

                break;
            }
            case MGMT_SYS_EVENT_TEARDOWN:
            {
                // Reset associate RTP session. 
                sx_mgmt_rtp_reset(msg.event_data.teardown.id); 

                f_cblk.active_session--; 

                if(f_cblk.active_session == 0)
                {
                    logger_log("mgmt_video_reset() Invoked");

                    sx_mgmt_video_reset(); 
                }
                break; 
            }
            default:
            {
                assert(0);
            }
        }
    }
}


static void mgmt_sys_thread_create(
    )
{
    pthread_create(&f_cblk.thread_id, NULL, (void *) &mgmt_sys_thread, NULL);
}


void mgmt_sys_init(
    )
{
    printf("mgmt_sys_init(): Invoked.\n"); 

    // Initialize resources.
    rerouces_init();

    // Initialize RTSP manager.
    sx_mgmt_rtsp_init(mgmt_rtsp_cback, &f_cblk);

    // Initialize RTP manager.
    sx_mgmt_rtp_init();

    // Initialize video manager. 
    sx_mgmt_video_init(); 
}


void mgmt_sys_open(
    )
{
    printf("mgmt_sys_open(): Invoked.\n"); 

    mgmt_sys_thread_create();

    pthread_join(f_cblk.thread_id, NULL);
}
