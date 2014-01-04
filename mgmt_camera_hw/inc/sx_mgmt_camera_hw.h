#ifndef _SW_CAMERA_HW_H_
#define _SW_CAMERA_HW_H_

#define SX_CAMERA_HW_NAL_LEN_MAX    (65536*2)

typedef struct
{
    unsigned char   nal[SX_CAMERA_HW_NAL_LEN_MAX];
    unsigned int    nal_len;

} sSX_CAMERA_HW_BUFFER;

extern void sx_camera_hw_open(
    void
    ); 

extern sSX_CAMERA_HW_BUFFER * sx_camera_hw_get(
    void
    );

#endif // #ifndef _SW_CAMERA_HW_H_
