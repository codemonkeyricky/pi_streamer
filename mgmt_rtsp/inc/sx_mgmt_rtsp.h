
typedef struct
{
    unsigned int    id; 
    unsigned int    ip; 
    unsigned short  port; 

} sSX_MGMT_RTSP_EVENT_DATA_PLAY; 


typedef struct
{
    unsigned int    id; 

} sSX_MGMT_RTSP_EVENT_DATA_TEARDOWN; 


typedef union
{
    sSX_MGMT_RTSP_EVENT_DATA_PLAY       play;
    sSX_MGMT_RTSP_EVENT_DATA_TEARDOWN   teardown;

} uSX_MGMT_RTSP_EVENT_DATA; 


typedef enum
{
    MGMT_RTSP_EVENT_PLAY, 
    MGMT_RTSP_EVENT_TEARDOWN
        
} eSX_MGMT_RTSP_EVENT; 


typedef void (*fSX_MGMT_RTSP_CBACK) (
    void                       *arg,
    eSX_MGMT_RTSP_EVENT         event,
    uSX_MGMT_RTSP_EVENT_DATA   *event_data
); 


extern void sx_mgmt_rtsp_init(
    fSX_MGMT_RTSP_CBACK    user_cback, 
    void               *user_arg
    ); 


extern void sx_mgmt_rtsp_open(
    void
    ); 

