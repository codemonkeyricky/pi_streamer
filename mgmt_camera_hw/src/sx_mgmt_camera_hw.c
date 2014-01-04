
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>

#include "sx_mgmt_camera_hw.h"
#include "sx_queue.h"

#define VERSION_STRING "v1.2"

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"

#define PREVIEW_LAYER      2
#define PREVIEW_FRAME_RATE_NUM 30
#define PREVIEW_FRAME_RATE_DEN 1

/// Camera number to use - we only have one camera, indexed from 0.
#define CAMERA_NUMBER 0

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Video format information
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

// Max bitrate we allow for recording
const int MAX_BITRATE = 30000000; // 30Mbits/s

/// Interval at which we check for an failure abort during capture
const int ABORT_INTERVAL = 100; // ms

// There isn't actually a MMAL structure for the following, so make one
typedef struct
{
   int enable;       /// Turn colourFX on or off
   int u,v;          /// U and V to use
} MMAL_PARAM_COLOURFX_T;

typedef struct
{
   int sharpness;             /// -100 to 100
   int contrast;              /// -100 to 100
   int brightness;            ///  0 to 100
   int saturation;            ///  -100 to 100
   int ISO;                   ///  TODO : what range?
   int videoStabilisation;    /// 0 or 1 (false or true)
   int exposureCompensation;  /// -10 to +10 ?
   MMAL_PARAM_EXPOSUREMODE_T exposureMode;
   MMAL_PARAM_EXPOSUREMETERINGMODE_T exposureMeterMode;
   MMAL_PARAM_AWBMODE_T awbMode;
   MMAL_PARAM_IMAGEFX_T imageEffect;
   MMAL_PARAMETER_IMAGEFX_PARAMETERS_T imageEffectsParameters;
   MMAL_PARAM_COLOURFX_T colourEffects;
   int rotation;              /// 0-359
   int hflip;                 /// 0 or 1
   int vflip;                 /// 0 or 1
} RASPICAM_CAMERA_PARAMETERS;



typedef struct
{
   int wantPreview;                       /// Display a preview
   int wantFullScreenPreview;             /// 0 is use previewRect, non-zero to use full screen
   int opacity;                           /// Opacity of window - 0 = transparent, 255 = opaque
   MMAL_RECT_T previewWindow;             /// Destination rectangle for the preview window.
   MMAL_COMPONENT_T *preview_component;   /// Pointer to the created preview display component
} RASPIPREVIEW_PARAMETERS;

typedef struct
{
   int timeout;                        /// Time taken before frame is grabbed and app then shuts down. Units are milliseconds
   int width;                          /// Requested width of image
   int height;                         /// requested height of image
   int bitrate;                        /// Requested bitrate
   int framerate;                      /// Requested frame rate (fps)
   int intraperiod;                    /// Intra-refresh period (key frame rate)
   char *filename;                     /// filename of output file
   int verbose;                        /// !0 if want detailed run information
   int demoMode;                       /// Run app in demo mode
   int demoInterval;                   /// Interval between camera settings changes
   int immutableInput;                /// Flag to specify whether encoder works in place or creates a new buffer. Result is preview can display either
                                       /// the camera output or the encoder output (with compression artifacts)
   RASPIPREVIEW_PARAMETERS preview_parameters;   /// Preview setup parameters
   RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

   MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
   MMAL_COMPONENT_T *encoder_component;   /// Pointer to the encoder component
   MMAL_CONNECTION_T *preview_connection; /// Pointer to the connection from camera to preview
   MMAL_CONNECTION_T *encoder_connection; /// Pointer to the connection from camera to encoder

   MMAL_POOL_T *encoder_pool; /// Pointer to the pool of buffers used by encoder output port

} RASPIVID_STATE;

typedef struct
{
   FILE *file_handle;                   /// File handle to write buffer data to.
   RASPIVID_STATE *pstate;              /// pointer to our state in case required in callback
   int abort;                           /// Set to 1 in callback if an error occurs to attempt to abort the capture
} PORT_USERDATA;


static unsigned int get_time_ns(
    void
    )
{
    struct timespec curr_time;


    // Get time.
    clock_gettime(CLOCK_REALTIME, &curr_time);

    return curr_time.tv_nsec;
}


static int mmal_status_to_int(
    MMAL_STATUS_T status
    )
{
    assert(status == MMAL_SUCCESS);

    return 0;
}

static SX_QUEUE f_nal_queue;

static MMAL_STATUS_T connect_ports(MMAL_PORT_T *output_port, MMAL_PORT_T *input_port, MMAL_CONNECTION_T **connection)
{
   MMAL_STATUS_T status;

   status =  mmal_connection_create(connection, output_port, input_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);

   if (status == MMAL_SUCCESS)
   {
      status =  mmal_connection_enable(*connection);
      if (status != MMAL_SUCCESS)
         mmal_connection_destroy(*connection);
   }

   return status;
}

static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
   }
   else
   {
      vcos_log_error("Received unexpected camera control callback event, 0x%08x", buffer->cmd);
   }

   mmal_buffer_header_release(buffer);
}

/**
 * Adjust the saturation level for images
 * @param camera Pointer to camera component
 * @param saturation Value to adjust, -100 to 100
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_saturation(MMAL_COMPONENT_T *camera, int saturation)
{
   int ret = 0;

   if (!camera)
      return 1;

   if (saturation >= -100 && saturation <= 100)
   {
      MMAL_RATIONAL_T value = {saturation, 100};

      MMAL_STATUS_T status;

      status = mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_SATURATION, value);
      assert(status == MMAL_SUCCESS);

      ret = 0;
   }
   else
   {
      vcos_log_error("Invalid saturation value");
      ret = 1;
   }

   return ret;
}

/**
 * Set the sharpness of the image
 * @param camera Pointer to camera component
 * @param sharpness Sharpness adjustment -100 to 100
 */
int raspicamcontrol_set_sharpness(MMAL_COMPONENT_T *camera, int sharpness)
{
   int ret = 0;

   if (!camera)
      return 1;

   if (sharpness >= -100 && sharpness <= 100)
   {
      MMAL_RATIONAL_T value = {sharpness, 100};
      ret = mmal_status_to_int(mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_SHARPNESS, value));
   }
   else
   {
      vcos_log_error("Invalid sharpness value");
      ret = 1;
   }

   return ret;
}

/**
 * Set the contrast adjustment for the image
 * @param camera Pointer to camera component
 * @param contrast Contrast adjustment -100 to  100
 * @return
 */
int raspicamcontrol_set_contrast(MMAL_COMPONENT_T *camera, int contrast)
{
   int ret = 0;

   if (!camera)
      return 1;

   if (contrast >= -100 && contrast <= 100)
   {
      MMAL_RATIONAL_T value = {contrast, 100};
      ret = mmal_status_to_int(mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_CONTRAST, value));
   }
   else
   {
      vcos_log_error("Invalid contrast value");
      ret = 1;
   }

   return ret;
}

/**
 * Adjust the brightness level for images
 * @param camera Pointer to camera component
 * @param brightness Value to adjust, 0 to 100
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_brightness(MMAL_COMPONENT_T *camera, int brightness)
{
   int ret = 0;

   if (!camera)
      return 1;

   if (brightness >= 0 && brightness <= 100)
   {
      MMAL_RATIONAL_T value = {brightness, 100};
      ret = mmal_status_to_int(mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_BRIGHTNESS, value));
   }
   else
   {
      vcos_log_error("Invalid brightness value");
      ret = 1;
   }

   return ret;
}

/**
 * Adjust the ISO used for images
 * @param camera Pointer to camera component
 * @param ISO Value to set TODO :
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_ISO(MMAL_COMPONENT_T *camera, int ISO)
{
   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_ISO, ISO));
}

/**
 * Adjust the metering mode for images
 * @param camera Pointer to camera component
 * @param saturation Value from following
 *   - MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE,
 *   - MMAL_PARAM_EXPOSUREMETERINGMODE_SPOT,
 *   - MMAL_PARAM_EXPOSUREMETERINGMODE_BACKLIT,
 *   - MMAL_PARAM_EXPOSUREMETERINGMODE_MATRIX
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_metering_mode(MMAL_COMPONENT_T *camera, MMAL_PARAM_EXPOSUREMETERINGMODE_T m_mode )
{
   MMAL_PARAMETER_EXPOSUREMETERINGMODE_T meter_mode = {{MMAL_PARAMETER_EXP_METERING_MODE,sizeof(meter_mode)},
                                                      m_mode};
   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set(camera->control, &meter_mode.hdr));
}


/**
 * Set the video stabilisation flag. Only used in video mode
 * @param camera Pointer to camera component
 * @param saturation Flag 0 off 1 on
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_video_stabilisation(MMAL_COMPONENT_T *camera, int vstabilisation)
{
   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set_boolean(camera->control, MMAL_PARAMETER_VIDEO_STABILISATION, vstabilisation));
}

/**
 * Adjust the exposure compensation for images (EV)
 * @param camera Pointer to camera component
 * @param exp_comp Value to adjust, -10 to +10
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_exposure_compensation(MMAL_COMPONENT_T *camera, int exp_comp)
{
   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set_int32(camera->control, MMAL_PARAMETER_EXPOSURE_COMP , exp_comp));
}


/**
 * Set exposure mode for images
 * @param camera Pointer to camera component
 * @param mode Exposure mode to set from
 *   - MMAL_PARAM_EXPOSUREMODE_OFF,
 *   - MMAL_PARAM_EXPOSUREMODE_AUTO,
 *   - MMAL_PARAM_EXPOSUREMODE_NIGHT,
 *   - MMAL_PARAM_EXPOSUREMODE_NIGHTPREVIEW,
 *   - MMAL_PARAM_EXPOSUREMODE_BACKLIGHT,
 *   - MMAL_PARAM_EXPOSUREMODE_SPOTLIGHT,
 *   - MMAL_PARAM_EXPOSUREMODE_SPORTS,
 *   - MMAL_PARAM_EXPOSUREMODE_SNOW,
 *   - MMAL_PARAM_EXPOSUREMODE_BEACH,
 *   - MMAL_PARAM_EXPOSUREMODE_VERYLONG,
 *   - MMAL_PARAM_EXPOSUREMODE_FIXEDFPS,
 *   - MMAL_PARAM_EXPOSUREMODE_ANTISHAKE,
 *   - MMAL_PARAM_EXPOSUREMODE_FIREWORKS,
 *
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_exposure_mode(MMAL_COMPONENT_T *camera, MMAL_PARAM_EXPOSUREMODE_T mode)
{
   MMAL_PARAMETER_EXPOSUREMODE_T exp_mode = {{MMAL_PARAMETER_EXPOSURE_MODE,sizeof(exp_mode)}, mode};

   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set(camera->control, &exp_mode.hdr));
}


/**
 * Set the aWB (auto white balance) mode for images
 * @param camera Pointer to camera component
 * @param awb_mode Value to set from
 *   - MMAL_PARAM_AWBMODE_OFF,
 *   - MMAL_PARAM_AWBMODE_AUTO,
 *   - MMAL_PARAM_AWBMODE_SUNLIGHT,
 *   - MMAL_PARAM_AWBMODE_CLOUDY,
 *   - MMAL_PARAM_AWBMODE_SHADE,
 *   - MMAL_PARAM_AWBMODE_TUNGSTEN,
 *   - MMAL_PARAM_AWBMODE_FLUORESCENT,
 *   - MMAL_PARAM_AWBMODE_INCANDESCENT,
 *   - MMAL_PARAM_AWBMODE_FLASH,
 *   - MMAL_PARAM_AWBMODE_HORIZON,
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_awb_mode(MMAL_COMPONENT_T *camera, MMAL_PARAM_AWBMODE_T awb_mode)
{
   MMAL_PARAMETER_AWBMODE_T param = {{MMAL_PARAMETER_AWB_MODE,sizeof(param)}, awb_mode};

   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set(camera->control, &param.hdr));
}

/**
 * Set the image effect for the images
 * @param camera Pointer to camera component
 * @param imageFX Value from
 *   - MMAL_PARAM_IMAGEFX_NONE,
 *   - MMAL_PARAM_IMAGEFX_NEGATIVE,
 *   - MMAL_PARAM_IMAGEFX_SOLARIZE,
 *   - MMAL_PARAM_IMAGEFX_POSTERIZE,
 *   - MMAL_PARAM_IMAGEFX_WHITEBOARD,
 *   - MMAL_PARAM_IMAGEFX_BLACKBOARD,
 *   - MMAL_PARAM_IMAGEFX_SKETCH,
 *   - MMAL_PARAM_IMAGEFX_DENOISE,
 *   - MMAL_PARAM_IMAGEFX_EMBOSS,
 *   - MMAL_PARAM_IMAGEFX_OILPAINT,
 *   - MMAL_PARAM_IMAGEFX_HATCH,
 *   - MMAL_PARAM_IMAGEFX_GPEN,
 *   - MMAL_PARAM_IMAGEFX_PASTEL,
 *   - MMAL_PARAM_IMAGEFX_WATERCOLOUR,
 *   - MMAL_PARAM_IMAGEFX_FILM,
 *   - MMAL_PARAM_IMAGEFX_BLUR,
 *   - MMAL_PARAM_IMAGEFX_SATURATION,
 *   - MMAL_PARAM_IMAGEFX_COLOURSWAP,
 *   - MMAL_PARAM_IMAGEFX_WASHEDOUT,
 *   - MMAL_PARAM_IMAGEFX_POSTERISE,
 *   - MMAL_PARAM_IMAGEFX_COLOURPOINT,
 *   - MMAL_PARAM_IMAGEFX_COLOURBALANCE,
 *   - MMAL_PARAM_IMAGEFX_CARTOON,
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_imageFX(MMAL_COMPONENT_T *camera, MMAL_PARAM_IMAGEFX_T imageFX)
{
   MMAL_PARAMETER_IMAGEFX_T imgFX = {{MMAL_PARAMETER_IMAGE_EFFECT,sizeof(imgFX)}, imageFX};

   if (!camera)
      return 1;

   return mmal_status_to_int(mmal_port_parameter_set(camera->control, &imgFX.hdr));
}

/* TODO :what to do with the image effects parameters?
   MMAL_PARAMETER_IMAGEFX_PARAMETERS_T imfx_param = {{MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS,sizeof(imfx_param)},
                              imageFX, 0, {0}};
mmal_port_parameter_set(camera->control, &imfx_param.hdr);
                             */

/**
 * Set the colour effect  for images (Set UV component)
 * @param camera Pointer to camera component
 * @param colourFX  Contains enable state and U and V numbers to set (e.g. 128,128 = Black and white)
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_colourFX(MMAL_COMPONENT_T *camera, const MMAL_PARAM_COLOURFX_T *colourFX)
{
   MMAL_PARAMETER_COLOURFX_T colfx = {{MMAL_PARAMETER_COLOUR_EFFECT,sizeof(colfx)}, 0, 0, 0};

   if (!camera)
      return 1;

   colfx.enable = colourFX->enable;
   colfx.u = colourFX->u;
   colfx.v = colourFX->v;

   return mmal_status_to_int(mmal_port_parameter_set(camera->control, &colfx.hdr));

}


/**
 * Set the rotation of the image
 * @param camera Pointer to camera component
 * @param rotation Degree of rotation (any number, but will be converted to 0,90,180 or 270 only)
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_rotation(MMAL_COMPONENT_T *camera, int rotation)
{
   int ret;
   int my_rotation = ((rotation % 360 ) / 90) * 90;

   ret = mmal_port_parameter_set_int32(camera->output[0], MMAL_PARAMETER_ROTATION, my_rotation);
   mmal_port_parameter_set_int32(camera->output[1], MMAL_PARAMETER_ROTATION, my_rotation);
   mmal_port_parameter_set_int32(camera->output[2], MMAL_PARAMETER_ROTATION, my_rotation);

   return ret;
}

/**
 * Set the flips state of the image
 * @param camera Pointer to camera component
 * @param hflip If true, horizontally flip the image
 * @param vflip If true, vertically flip the image
 *
 * @return 0 if successful, non-zero if any parameters out of range
 */
int raspicamcontrol_set_flips(MMAL_COMPONENT_T *camera, int hflip, int vflip)
{
   MMAL_PARAMETER_MIRROR_T mirror = {{MMAL_PARAMETER_MIRROR, sizeof(MMAL_PARAMETER_MIRROR_T)}, MMAL_PARAM_MIRROR_NONE};

   if (hflip && vflip)
      mirror.value = MMAL_PARAM_MIRROR_BOTH;
   else
   if (hflip)
      mirror.value = MMAL_PARAM_MIRROR_HORIZONTAL;
   else
   if (vflip)
      mirror.value = MMAL_PARAM_MIRROR_VERTICAL;

   mmal_port_parameter_set(camera->output[0], &mirror.hdr);
   mmal_port_parameter_set(camera->output[1], &mirror.hdr);
   return mmal_port_parameter_set(camera->output[2], &mirror.hdr);
}

int raspicamcontrol_set_all_parameters(MMAL_COMPONENT_T *camera, const RASPICAM_CAMERA_PARAMETERS *params)
{
   int result;

   result  = raspicamcontrol_set_saturation(camera, params->saturation);
   result += raspicamcontrol_set_sharpness(camera, params->sharpness);
   result += raspicamcontrol_set_contrast(camera, params->contrast);
   result += raspicamcontrol_set_brightness(camera, params->brightness);
   result += raspicamcontrol_set_ISO(camera, params->ISO);
   result += raspicamcontrol_set_video_stabilisation(camera, params->videoStabilisation);
   result += raspicamcontrol_set_exposure_compensation(camera, params->exposureCompensation);
   result += raspicamcontrol_set_exposure_mode(camera, params->exposureMode);
   result += raspicamcontrol_set_metering_mode(camera, params->exposureMeterMode);
   result += raspicamcontrol_set_awb_mode(camera, params->awbMode);
   result += raspicamcontrol_set_imageFX(camera, params->imageEffect);
   result += raspicamcontrol_set_colourFX(camera, &params->colourEffects);
   //result += raspicamcontrol_set_thumbnail_parameters(camera, &params->thumbnailConfig);  TODO Not working for some reason
   result += raspicamcontrol_set_rotation(camera, params->rotation);
   result += raspicamcontrol_set_flips(camera, params->hflip, params->vflip);

   return result;
}


void raspipreview_set_defaults(RASPIPREVIEW_PARAMETERS *state)
{
   state->wantPreview = 1;
   state->wantFullScreenPreview = 1;
   state->opacity = 255;
   state->previewWindow.x = 0;
   state->previewWindow.y = 0;
   state->previewWindow.width = 1024;
   state->previewWindow.height = 768;
   state->preview_component = NULL;
}


void raspicamcontrol_set_defaults(RASPICAM_CAMERA_PARAMETERS *params)
{
   vcos_assert(params);

   params->sharpness = 0;
   params->contrast = 0;
   params->brightness = 50;
   params->saturation = 0;
   params->ISO = 400;
   params->videoStabilisation = 0;
   params->exposureCompensation = 0;
   params->exposureMode = MMAL_PARAM_EXPOSUREMODE_AUTO;
   params->exposureMeterMode = MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE;
   params->awbMode = MMAL_PARAM_AWBMODE_AUTO;
   params->imageEffect = MMAL_PARAM_IMAGEFX_NONE;
   params->colourEffects.enable = 0;
   params->colourEffects.u = 128;
   params->colourEffects.v = 128;
   params->rotation = 0;
   params->hflip = params->vflip = 0;
}


static void default_status(
    RASPIVID_STATE *state
    )
{
   if (!state)
   {
      vcos_assert(0);
      return;
   }

   // Default everything to zero
   memset(state, 0, sizeof(RASPIVID_STATE));

   // Now set anything non-zero
   state->timeout = 5000;     // 5s delay before take image
   state->width = 1280;       // Default to 1080p
   state->height = 720;
   state->bitrate = 1000000; // This is a decent default bitrate for 1080p
   state->framerate = 30;
   state->intraperiod = 0;    // Not set
   state->demoMode = 0;
   state->demoInterval = 250; // ms
   state->immutableInput = 1;

   // Setup preview window defaults
   raspipreview_set_defaults(&state->preview_parameters);

   // Set up the camera_parameters to default
   raspicamcontrol_set_defaults(&state->camera_parameters);
}


static void encoder_buffer_callback(
    MMAL_PORT_T *port,
    MMAL_BUFFER_HEADER_T *buffer
    )
{
    MMAL_BUFFER_HEADER_T *new_buffer;

    static int buffer_count = 0;
    static sSX_CAMERA_HW_BUFFER *hw_buf = NULL;
    static unsigned index = 0;

    // We pass our file handle and other stuff in via the userdata field.


    printf("callback current time = %d\n", get_time_ns());


    PORT_USERDATA *pData = (PORT_USERDATA *) port->userdata;
    assert(pData != NULL);

    int bytes_written = buffer->length;

    vcos_assert(pData->file_handle);

    assert(buffer->length > 0);

    // Get frame end.
    unsigned char frame_end = 0;
    if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
    {
        frame_end = 1;
    }

    unsigned char config = 0;
    if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
    {
        config = 1;
    }

#if 0
    printf("frame_end = %d\n", buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END);

    printf("config = %d\n", buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG);

    printf("buffer received: len = %d, frame_end = %d\n",
            buffer->length, frame_end);
#endif

    if(hw_buf == NULL)
    {
        index = 0;
        hw_buf = malloc(sizeof(sSX_CAMERA_HW_BUFFER));
    }

    mmal_buffer_header_mem_lock(buffer);

    unsigned int offset = 0;
    if(   (buffer->data[0] == 0x00)
       && (buffer->data[1] == 0x00)
       && (buffer->data[2] == 0x00)
       && (buffer->data[3] == 0x01))
    {
        offset = 4;
    }

    assert((index + buffer->length) <= SX_CAMERA_HW_NAL_LEN_MAX);

    memcpy(&hw_buf->nal[index],
            buffer->data + offset,
            buffer->length - offset);

    index += (buffer->length - offset);

    mmal_buffer_header_mem_unlock(buffer);

    // release buffer back to the pool
    mmal_buffer_header_release(buffer);

    if(frame_end || config)
    {
        hw_buf->nal_len = index;

        sx_queue_push(f_nal_queue, hw_buf);

        hw_buf = NULL;

//        printf("(sx_camera_hw): Queue nal unit [len = %d]\n", index);
    }


#if 0
    printf("frame end = %d\n",
            frame_end);

    printf("data len = %d\n",
            buffer->length);

    printf("0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
            buffer->data[0],
            buffer->data[1],
            buffer->data[2],
            buffer->data[3],
            buffer->data[4],
            buffer->data[5],
            buffer->data[6],
            buffer->data[7]);


    mmal_buffer_header_mem_lock(buffer);

    bytes_written = fwrite(buffer->data, 1, buffer->length, pData->file_handle);

    mmal_buffer_header_mem_unlock(buffer);
#endif

    // and send one back to the port (if still open)
    if (port->is_enabled)
    {
        MMAL_STATUS_T status;

        new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);
        if (new_buffer)
        {
            status = mmal_port_send_buffer(port, new_buffer);
            assert(status == MMAL_SUCCESS);
        }
    }
}


MMAL_STATUS_T raspipreview_create(
    RASPIPREVIEW_PARAMETERS *state
    )
{
   MMAL_COMPONENT_T *preview = 0;
   MMAL_PORT_T *preview_port = NULL;
   MMAL_STATUS_T status;


   // No preview required, so create a null sink component to take its place
   status = mmal_component_create("vc.null_sink", &preview);

   if (status != MMAL_SUCCESS)
   {
       vcos_log_error("Unable to create null sink component");
       goto error;
   }

   /* Enable component */
   status = mmal_component_enable(preview);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable preview/null sink component (%u)", status);
      goto error;
   }

   state->preview_component = preview;

   return status;

error:

   if (preview)
      mmal_component_destroy(preview);

   return status;
}

static MMAL_STATUS_T create_camera_component(
    RASPIVID_STATE *state
    )
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
   MMAL_STATUS_T status;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Failed to create camera component");
      goto error;
   }

   if (!camera->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Camera doesn't have output ports");
      goto error;
   }

   preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
   video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
   still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

   // Enable the camera, and tell it its control callback function
   status = mmal_port_enable(camera->control, camera_control_callback);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable control port : error %d", status);
      goto error;
   }

   //  set up the camera configuration
   {
      MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
      {
         { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
         .max_stills_w = state->width,
         .max_stills_h = state->height,
         .stills_yuv422 = 0,
         .one_shot_stills = 0,
         .max_preview_video_w = state->width,
         .max_preview_video_h = state->height,
         .num_preview_video_frames = 3,
         .stills_capture_circular_buffer_height = 0,
         .fast_preview_resume = 0,
         .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
      };
      mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   // Now set up the port formats

   // Set the encode format on the Preview port
   // HW limitations mean we need the preview to be the same size as the required recorded output

   format = preview_port->format;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = state->width;
   format->es->video.height = state->height;
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = state->framerate;
   format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

   status = mmal_port_format_commit(preview_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera viewfinder format couldn't be set");
      goto error;
   }

   // Set the encode format on the video  port

   format = video_port->format;
   format->encoding_variant = MMAL_ENCODING_I420;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = state->width;
   format->es->video.height = state->height;
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = state->framerate;
   format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

   status = mmal_port_format_commit(video_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera video format couldn't be set");
      goto error;
   }

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;


   // Set the encode format on the still  port

   format = still_port->format;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   format->es->video.width = state->width;
   format->es->video.height = state->height;
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = 1;
   format->es->video.frame_rate.den = 1;

   status = mmal_port_format_commit(still_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera still format couldn't be set");
      goto error;
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   status = mmal_component_enable(camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera component couldn't be enabled");
      goto error;
   }

   raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

   state->camera_component = camera;

   if (state->verbose)
      fprintf(stderr, "Camera component done\n");

   return status;

error:

   if (camera)
      mmal_component_destroy(camera);

   return status;
}


static MMAL_STATUS_T create_encoder_component(RASPIVID_STATE *state)
{
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
   MMAL_STATUS_T status;
   MMAL_POOL_T *pool;

   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to create video encoder component");
      goto error;
   }

   if (!encoder->input_num || !encoder->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Video encoder doesn't have input/output ports");
      goto error;
   }

   encoder_input = encoder->input[0];
   encoder_output = encoder->output[0];

   // We want same format on input and output
   mmal_format_copy(encoder_output->format, encoder_input->format);

   // Only supporting H264 at the moment
   encoder_output->format->encoding = MMAL_ENCODING_H264;

   encoder_output->format->bitrate = state->bitrate;

   encoder_output->buffer_size = encoder_output->buffer_size_recommended;

   if (encoder_output->buffer_size < encoder_output->buffer_size_min)
      encoder_output->buffer_size = encoder_output->buffer_size_min;

   encoder_output->buffer_num = encoder_output->buffer_num_recommended;

   if (encoder_output->buffer_num < encoder_output->buffer_num_min)
      encoder_output->buffer_num = encoder_output->buffer_num_min;

   // Commit the port changes to the output port
   status = mmal_port_format_commit(encoder_output);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set format on video encoder output port");
      goto error;
   }


   // Set the rate control parameter
   if (0)
   {
      MMAL_PARAMETER_VIDEO_RATECONTROL_T param = {{ MMAL_PARAMETER_RATECONTROL, sizeof(param)}, MMAL_VIDEO_RATECONTROL_DEFAULT};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set ratecontrol");
         goto error;
      }

   }

   if (state->intraperiod)
   {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, state->intraperiod};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set intraperiod");
         goto error;
      }

   }

   if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, state->immutableInput) != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set immutable input flag");
      // Continue rather than abort..
   }

   //  Enable component
   status = mmal_component_enable(encoder);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable video encoder component");
      goto error;
   }

   /* Create pool of buffer headers for the output port to consume */
   pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

   if (!pool)
   {
      vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
   }

   state->encoder_pool = pool;
   state->encoder_component = encoder;

   if (state->verbose)
      fprintf(stderr, "Encoder component done\n");

   return status;

   error:
   if (encoder)
      mmal_component_destroy(encoder);

   return status;
}


static pthread_t    f_thread_id;



void camera_thread(
    void
    )
{
    static MMAL_STATUS_T   status;
    static RASPIVID_STATE state;
    static MMAL_PORT_T *camera_preview_port = NULL;
    static MMAL_PORT_T *camera_video_port = NULL;
    static MMAL_PORT_T *camera_still_port = NULL;
    static MMAL_PORT_T *preview_input_port = NULL;
    static MMAL_PORT_T *encoder_input_port = NULL;
    static MMAL_PORT_T *encoder_output_port = NULL;
    static FILE *output_file = NULL;


    f_nal_queue = sx_queue_create();

    bcm_host_init();

    default_status(&state);

    state.verbose = 1;

    status = create_camera_component(&state);
    assert(status == MMAL_SUCCESS);

    status = raspipreview_create(&state.preview_parameters);
    assert(status == MMAL_SUCCESS);

    status = create_encoder_component(&state);
    assert(status == MMAL_SUCCESS);

    PORT_USERDATA callback_data;

    camera_preview_port = state.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port   = state.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port   = state.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
    preview_input_port  = state.preview_parameters.preview_component->input[0];
    encoder_input_port  = state.encoder_component->input[0];
    encoder_output_port = state.encoder_component->output[0];

    // Now connect the camera to the encoder
    status = connect_ports(camera_video_port,
                           encoder_input_port,
                           &state.encoder_connection);
    assert(status == MMAL_SUCCESS);

    char *filename = "output.h264";

    state.filename = filename;

    output_file = fopen(state.filename, "wb");

    // Set up our userdata - this is passed though to the callback where we need the information.
    callback_data.file_handle = output_file;
    callback_data.pstate = &state;
    callback_data.abort = 0;

    encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *) &callback_data;

    // Enable the encoder output port and tell it its callback function
    status = mmal_port_enable(encoder_output_port, encoder_buffer_callback);
    assert(status == MMAL_SUCCESS);

    // Only encode stuff if we have a filename and it opened
    if (output_file)
    {
        int wait;

        if (state.verbose)
            fprintf(stderr, "Starting video capture\n");

        status = mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1);
        assert(status == MMAL_SUCCESS);

        // Send all the buffers to the encoder output port
        {
            int num = mmal_queue_length(state.encoder_pool->queue);
            int q;

            fprintf(stderr, "mmal_queue_len = %d\n", num);

            for (q=0;q<num;q++)
            {
                printf("start current time = %d\n", get_time_ns());

                MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state.encoder_pool->queue);
                assert(buffer != NULL);

                status = mmal_port_send_buffer(encoder_output_port, buffer);
                assert(status == MMAL_SUCCESS);
            }
        }

        // Now wait until we need to stop. Whilst waiting we do need to check to see if we have aborted (for example
        // out of storage space)
        // Going to check every ABORT_INTERVAL milliseconds

#if 1
        while(1)
        {
            // sleep forever.
            vcos_sleep(ABORT_INTERVAL);
        }
#endif

#if 0
        for (wait = 0; state.timeout == 0 || wait < state.timeout; wait+= ABORT_INTERVAL)
        {
            vcos_sleep(ABORT_INTERVAL);
            if (callback_data.abort)
            {
                fprintf(stderr, "333\n");

                break;
            }

            fprintf(stderr, "222\n");
        }

        fprintf(stderr, "wait = %u\n", wait);

        if (state.verbose)
            fprintf(stderr, "Finished capture\n");
#endif
    }
}


void sx_camera_hw_open(
    void
    )
{
    printf("#### n");

    pthread_create(&f_thread_id, NULL, (void *) &camera_thread, NULL);
}


sSX_CAMERA_HW_BUFFER * sx_camera_hw_get(
    void
    )
{
    if(f_nal_queue == NULL)
    {
        return NULL;
    }

    unsigned int len = sx_queue_len_get(f_nal_queue);
    printf("f_nal_queue len = %d\n", len);

    return (sSX_CAMERA_HW_BUFFER *) sx_queue_pull(f_nal_queue);
}
