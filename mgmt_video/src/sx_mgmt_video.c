#include <stdio.h> 
#include <stdlib.h> 
#include "logger.h"
#include "pthread.h"
#include "mqueue.h"
#include "assert.h"

#include "sx_queue.h"
#include "sx_mgmt_video.h"
#include "sx_mgmt_camera_hw.h"

#define MGMT_VIDEO_MSG_QUEUE  "/mgmt_video_msg_queue"

typedef enum 
{
    MGMT_VIDEO_STATE_INIT, 
    MGMT_VIDEO_STATE_ACTIVE 

} eMGMT_VIDEO_STATE; 


typedef struct
{
    pthread_mutex_t         pps_mutex;      ///< NAL unit chain mutex.
    pthread_mutex_t         sps_mutex;      ///< NAL unit chain mutex.

    sMGMT_VIDEO_NAL_UNIT   *pps_nal_unit;   ///< Current PPS NAL unit.
    sMGMT_VIDEO_NAL_UNIT   *sps_nal_unit;   ///< Current SPS NAL unit.

    pthread_t               thread_id;      ///< Thread ID.
    mqd_t                   msg_queue;      ///< Message queue.

    eMGMT_VIDEO_STATE       state;          ///< State

    SX_QUEUE               nal_unit_queue;

} sMGMT_VIDEO_CBLK; 


typedef enum
{
   MGMT_VIDEO_EVENT_SERVICE, 
   MGMT_VIDEO_EVENT_ACTIVATE, 
   MGMT_VIDEO_EVENT_RESET,

} eMGMT_VIDEO_EVENT;


typedef struct
{
    unsigned int    placeholder; 

} sMGMT_SYS_EVENT_DATA_ACTIVATE; 


typedef struct
{
    sMGMT_VIDEO_NAL_UNIT   *nal_unit; 

} sMGMT_SYS_EVENT_DATA_SERVICE; 


typedef union
{
    sMGMT_SYS_EVENT_DATA_SERVICE    service; 
    sMGMT_SYS_EVENT_DATA_ACTIVATE   activate; 

} uMGMT_VIDEO_EVENT_DATA; 


typedef struct
{
    eMGMT_VIDEO_EVENT         event;
    uMGMT_VIDEO_EVENT_DATA    event_data;

} sMGMT_VIDEO_MSG; 


// Video manager control block. 
static sMGMT_VIDEO_CBLK f_cblk; 


// Initialize relevant system resources. 
static void resources_init(
    void
    )
{
    struct mq_attr queue_attr;


    // Set message attributes.
    queue_attr.mq_flags     = 0;
    queue_attr.mq_maxmsg    = 10;
    queue_attr.mq_msgsize   = sizeof(sMGMT_VIDEO_MSG);
    queue_attr.mq_curmsgs   = 0;

    // Create message queue.
    f_cblk.msg_queue = mq_open(MGMT_VIDEO_MSG_QUEUE,
                               O_CREAT | O_RDWR,
                               0644,
                               &queue_attr);

    // Initialize mutex. 
    pthread_mutex_init(&f_cblk.pps_mutex, NULL); 

    // Initialize mutex. 
    pthread_mutex_init(&f_cblk.sps_mutex, NULL); 

    f_cblk.nal_unit_queue = sx_queue_create();
}


// Activate handler. 
void idle_state_activate_event_handler(
    void
    )
{
    f_cblk.state = MGMT_VIDEO_STATE_ACTIVE; 
}


static void active_state_service_event_handler(
    sMGMT_VIDEO_MSG    *msg
    )
{
    sx_queue_push(f_cblk.nal_unit_queue, msg->event_data.service.nal_unit);
}


static void idle_state_service_event_handler(
    sMGMT_VIDEO_MSG    *msg
    )
{
    if((msg->event_data.service.nal_unit->nal_unit[0] & 0x1F) == 0x07)
    {
        // Cache SPS. 
        logger_log("MGMT_VIDEO:: New SPS NAL unit"); 

        // Lock mutex. 
        pthread_mutex_lock(&f_cblk.sps_mutex); 

        if(f_cblk.sps_nal_unit != NULL)
        {
            free(f_cblk.sps_nal_unit); 
        }

        // Free mutex. 
        f_cblk.sps_nal_unit = msg->event_data.service.nal_unit; 

        logger_log("MGMT_VIDEO:: New SPS NAL unit len %d", f_cblk.sps_nal_unit->nal_unit_len); 

        pthread_mutex_unlock(&f_cblk.sps_mutex); 
    }
    else if((msg->event_data.service.nal_unit->nal_unit[0] & 0x1F) == 0x08)
    {
        // Cache PPS. 
        logger_log("MGMT_VIDEO:: New PPS NAL unit"); 

        pthread_mutex_lock(&f_cblk.pps_mutex); 

        if(f_cblk.pps_nal_unit != NULL)
        {
            free(f_cblk.pps_nal_unit); 
        }

        pthread_mutex_unlock(&f_cblk.pps_mutex); 

        f_cblk.pps_nal_unit = msg->event_data.service.nal_unit;
    }
    else
    {
        // Free unit. 
        free(msg->event_data.service.nal_unit); 
    }
}


static void active_state_reset_event_handler(
    sMGMT_VIDEO_MSG    *msg
    )
{
    sMGMT_VIDEO_NAL_UNIT   *nal_unit;


    while(1)
    {
        nal_unit = sx_queue_pull(f_cblk.nal_unit_queue);
        if(nal_unit == NULL)
        {
            break;
        }

        free(nal_unit);
    }

    f_cblk.state = MGMT_VIDEO_STATE_INIT; 
}


static void idle_state_handler(
    sMGMT_VIDEO_MSG    *msg
    )
{
    switch(msg->event)
    {
        case MGMT_VIDEO_EVENT_SERVICE:
        {
//            logger_log("(idle_state_handler): msg->event_data.service.nal_unit = 0x%x",
//                       msg->event_data.service.nal_unit);
            
            idle_state_service_event_handler(msg);
            break;
        }
        case MGMT_VIDEO_EVENT_ACTIVATE:
        {
            idle_state_activate_event_handler(); 
            break;
        }
        case MGMT_VIDEO_EVENT_RESET:
        {
            break; 
        }
    }
}


static void active_state_handler(
    sMGMT_VIDEO_MSG    *msg
    )
{
    switch(msg->event)
    {
        case MGMT_VIDEO_EVENT_SERVICE:
            
            active_state_service_event_handler(msg); 
            break;

        case MGMT_VIDEO_EVENT_ACTIVATE:

            break;

        case MGMT_VIDEO_EVENT_RESET:

            logger_log("RESET Received");

            active_state_reset_event_handler(msg); 
            break; 
    }
}


static void mgmt_video_thread(
    void * arg
    )
{
    sMGMT_VIDEO_MSG msg; 
    unsigned int    bytes_read; 


    while(1)
    {
        bytes_read = mq_receive(f_cblk.msg_queue,
                                (char *) &msg,
                                sizeof(sMGMT_VIDEO_MSG),
                                NULL);
        assert(bytes_read > 0);

        switch(f_cblk.state)
        {
            case MGMT_VIDEO_STATE_INIT:
                idle_state_handler(&msg); 
                break; 

            case MGMT_VIDEO_STATE_ACTIVE:
                active_state_handler(&msg); 
                break; 

            default: 
                break; 
        }
    }
}


static void video_capture_thread(
    void *arg 
    )
{
    int                     status;
    unsigned int            index;
    unsigned int            curr_dword;
    unsigned char           temp;
    sMGMT_VIDEO_MSG         msg;
    sMGMT_VIDEO_NAL_UNIT   *nal_unit; 
    unsigned char           file_end; 


    while(1)
    {
        sSX_CAMERA_HW_BUFFER * hw_buf;
        do
        {
            usleep(5*1000);

            hw_buf = sx_camera_hw_get();

        } while (hw_buf == NULL);

        // Construct and queue service event.
        msg.event                       = MGMT_VIDEO_EVENT_SERVICE;
        msg.event_data.service.nal_unit = (sMGMT_VIDEO_NAL_UNIT *) hw_buf;

        mq_send(f_cblk.msg_queue,
                (char *) &msg,
                sizeof(sMGMT_VIDEO_MSG),
                0);
    }
}


static void mgmt_video_thread_create(
    void
    )
{
    pthread_create(&f_cblk.thread_id, NULL, (void *) &mgmt_video_thread, NULL);
}


static void video_capture_thread_create(
    void
    )
{
    pthread_create(&f_cblk.thread_id, NULL, (void *) &video_capture_thread, NULL);
}


void sx_mgmt_video_init(
    void
    )
{
    logger_log("(mgmt_video_init): Invoked."); 


    // Initialize resources. 
    resources_init(); 
}


void sx_mgmt_video_open(
    void
    )
{
    logger_log("(mgmt_video_open): Invoked."); 

    // Create video manager thread. 
    mgmt_video_thread_create(); 

    // Create video capturer thread. 
    video_capture_thread_create();

    sx_camera_hw_open();
}


void sx_mgmt_video_activate(
    void
    )
{
    sMGMT_VIDEO_MSG  msg; 


    logger_log("(mgmt_video_activate): Invoked."); 

    msg.event = MGMT_VIDEO_EVENT_ACTIVATE; 

    mq_send(f_cblk.msg_queue,
            (char *) &msg,
            sizeof(sMGMT_VIDEO_MSG),
            0);
}


void sx_mgmt_video_reset(
    void
    )
{
    sMGMT_VIDEO_MSG  msg; 


    logger_log("mgmt_video_reset(): Invoked."); 

    msg.event = MGMT_VIDEO_EVENT_RESET; 

    mq_send(f_cblk.msg_queue,
            (char *) &msg,
            sizeof(sMGMT_VIDEO_MSG),
            0);
}


unsigned char sx_mgmt_video_is_key_frame(
    sMGMT_VIDEO_NAL_UNIT   *nal_unit
    )
{
    if((nal_unit->nal_unit[0] & 0x1F) == 0x05) 
    {
        return 1; 
    }

    return 0; 
}


sMGMT_VIDEO_NAL_UNIT * sx_mgmt_video_pps_get(
    void
    )
{
    sMGMT_VIDEO_NAL_UNIT       *nal_unit; 


    logger_log("(mgmt_video_get_pps_nal_unit): Invoked.\n");

    // Mutex lock. 
    pthread_mutex_lock(&f_cblk.pps_mutex); 

    if(f_cblk.pps_nal_unit == NULL) 
    {
        nal_unit = NULL; 

        goto cleanup; 
    }

    nal_unit = malloc(sizeof(sMGMT_VIDEO_NAL_UNIT)); 

    // Copy PPS NAL unit. 
    *nal_unit = *f_cblk.pps_nal_unit; 

cleanup:
    // Mutex unlock. 
    pthread_mutex_unlock(&f_cblk.pps_mutex); 

    return nal_unit; 
}


sMGMT_VIDEO_NAL_UNIT * sx_mgmt_video_sps_get(
    void
    )
{
    sMGMT_VIDEO_NAL_UNIT   *nal_unit; 


    logger_log("mgmt_video_get_sps_nal_unit(): Invoked.\n"); 

    // Mutex lock. 
    pthread_mutex_lock(&f_cblk.sps_mutex); 

    if(f_cblk.sps_nal_unit == NULL) 
    {
        nal_unit = NULL; 

        goto cleanup; 
    }

    nal_unit = malloc(sizeof(sMGMT_VIDEO_NAL_UNIT)); 

    // Copy PPS NAL unit. 
    *nal_unit = *f_cblk.sps_nal_unit; 

    logger_log("nal unit %d", nal_unit->nal_unit_len); 

cleanup:
    // Mutex unlock. 
    pthread_mutex_unlock(&f_cblk.sps_mutex); 

    return nal_unit; 
}
    

sMGMT_VIDEO_NAL_UNIT * sx_mgmt_video_get_nal_unit(
    void
    )
{
    return sx_queue_pull(f_cblk.nal_unit_queue);
}


void sx_mgmt_video_free_nal_unit(
    sMGMT_VIDEO_NAL_UNIT   *nal_unit
    )
{
    free(nal_unit); 
}
