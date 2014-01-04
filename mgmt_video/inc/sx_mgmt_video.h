
#define NAL_UNIT_SIZE_MAX   (65536*2)


typedef struct
{
    unsigned char   nal_unit[NAL_UNIT_SIZE_MAX]; 
    unsigned int    nal_unit_len; 

} sMGMT_VIDEO_NAL_UNIT; 


extern void sx_mgmt_video_init(
    void
    ); 


extern void sx_mgmt_video_open(
    void
    ); 


extern void sx_mgmt_video_activate(
    void
    ); 


extern void sx_mgmt_video_reset(
    void
    ); 


extern sMGMT_VIDEO_NAL_UNIT * sx_mgmt_video_pps_get(
    void
    ); 


extern sMGMT_VIDEO_NAL_UNIT * sx_mgmt_video_sps_get(
    void
    ); 


extern sMGMT_VIDEO_NAL_UNIT * sx_mgmt_video_get_nal_unit(
    void
    ); 


extern unsigned char sx_mgmt_video_is_key_frame(
    sMGMT_VIDEO_NAL_UNIT   *nal_unit
    ); 


extern void sx_mgmt_video_free_nal_unit(
    sMGMT_VIDEO_NAL_UNIT   *nal_unit
    ); 

