.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _mpeg-controls:

***********************
Codec Control Reference
***********************

Below all controls within the Codec control class are described. First
the generic controls, then controls specific for certain hardware.

.. note::

   These controls are applicable to all codecs and not just MPEG. The
   defines are prefixed with V4L2_CID_MPEG/V4L2_MPEG as the controls
   were originally made for MPEG codecs and later extended to cover all
   encoding formats.


Generic Codec Controls
======================


.. _mpeg-control-id:

Codec Control IDs
-----------------

``V4L2_CID_MPEG_CLASS (class)``
    The Codec class descriptor. Calling
    :ref:`VIDIOC_QUERYCTRL` for this control will
    return a description of this control class. This description can be
    used as the caption of a Tab page in a GUI, for example.

.. _v4l2-mpeg-stream-type:

``V4L2_CID_MPEG_STREAM_TYPE``
    (enum)

enum v4l2_mpeg_stream_type -
    The MPEG-1, -2 or -4 output stream type. One cannot assume anything
    here. Each hardware MPEG encoder tends to support different subsets
    of the available MPEG stream types. This control is specific to
    multiplexed MPEG streams. The currently defined stream types are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_STREAM_TYPE_MPEG2_PS``
      - MPEG-2 program stream
    * - ``V4L2_MPEG_STREAM_TYPE_MPEG2_TS``
      - MPEG-2 transport stream
    * - ``V4L2_MPEG_STREAM_TYPE_MPEG1_SS``
      - MPEG-1 system stream
    * - ``V4L2_MPEG_STREAM_TYPE_MPEG2_DVD``
      - MPEG-2 DVD-compatible stream
    * - ``V4L2_MPEG_STREAM_TYPE_MPEG1_VCD``
      - MPEG-1 VCD-compatible stream
    * - ``V4L2_MPEG_STREAM_TYPE_MPEG2_SVCD``
      - MPEG-2 SVCD-compatible stream



``V4L2_CID_MPEG_STREAM_PID_PMT (integer)``
    Program Map Table Packet ID for the MPEG transport stream (default
    16)

``V4L2_CID_MPEG_STREAM_PID_AUDIO (integer)``
    Audio Packet ID for the MPEG transport stream (default 256)

``V4L2_CID_MPEG_STREAM_PID_VIDEO (integer)``
    Video Packet ID for the MPEG transport stream (default 260)

``V4L2_CID_MPEG_STREAM_PID_PCR (integer)``
    Packet ID for the MPEG transport stream carrying PCR fields (default
    259)

``V4L2_CID_MPEG_STREAM_PES_ID_AUDIO (integer)``
    Audio ID for MPEG PES

``V4L2_CID_MPEG_STREAM_PES_ID_VIDEO (integer)``
    Video ID for MPEG PES

.. _v4l2-mpeg-stream-vbi-fmt:

``V4L2_CID_MPEG_STREAM_VBI_FMT``
    (enum)

enum v4l2_mpeg_stream_vbi_fmt -
    Some cards can embed VBI data (e. g. Closed Caption, Teletext) into
    the MPEG stream. This control selects whether VBI data should be
    embedded, and if so, what embedding method should be used. The list
    of possible VBI formats depends on the driver. The currently defined
    VBI format types are:



.. tabularcolumns:: |p{6.6 cm}|p{10.9cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_STREAM_VBI_FMT_NONE``
      - No VBI in the MPEG stream
    * - ``V4L2_MPEG_STREAM_VBI_FMT_IVTV``
      - VBI in private packets, IVTV format (documented in the kernel
	sources in the file
	``Documentation/media/v4l-drivers/cx2341x.rst``)



.. _v4l2-mpeg-audio-sampling-freq:

``V4L2_CID_MPEG_AUDIO_SAMPLING_FREQ``
    (enum)

enum v4l2_mpeg_audio_sampling_freq -
    MPEG Audio sampling frequency. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_SAMPLING_FREQ_44100``
      - 44.1 kHz
    * - ``V4L2_MPEG_AUDIO_SAMPLING_FREQ_48000``
      - 48 kHz
    * - ``V4L2_MPEG_AUDIO_SAMPLING_FREQ_32000``
      - 32 kHz



.. _v4l2-mpeg-audio-encoding:

``V4L2_CID_MPEG_AUDIO_ENCODING``
    (enum)

enum v4l2_mpeg_audio_encoding -
    MPEG Audio encoding. This control is specific to multiplexed MPEG
    streams. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_ENCODING_LAYER_1``
      - MPEG-1/2 Layer I encoding
    * - ``V4L2_MPEG_AUDIO_ENCODING_LAYER_2``
      - MPEG-1/2 Layer II encoding
    * - ``V4L2_MPEG_AUDIO_ENCODING_LAYER_3``
      - MPEG-1/2 Layer III encoding
    * - ``V4L2_MPEG_AUDIO_ENCODING_AAC``
      - MPEG-2/4 AAC (Advanced Audio Coding)
    * - ``V4L2_MPEG_AUDIO_ENCODING_AC3``
      - AC-3 aka ATSC A/52 encoding



.. _v4l2-mpeg-audio-l1-bitrate:

``V4L2_CID_MPEG_AUDIO_L1_BITRATE``
    (enum)

enum v4l2_mpeg_audio_l1_bitrate -
    MPEG-1/2 Layer I bitrate. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_32K``
      - 32 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_64K``
      - 64 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_96K``
      - 96 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_128K``
      - 128 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_160K``
      - 160 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_192K``
      - 192 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_224K``
      - 224 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_256K``
      - 256 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_288K``
      - 288 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_320K``
      - 320 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_352K``
      - 352 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_384K``
      - 384 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_416K``
      - 416 kbit/s
    * - ``V4L2_MPEG_AUDIO_L1_BITRATE_448K``
      - 448 kbit/s



.. _v4l2-mpeg-audio-l2-bitrate:

``V4L2_CID_MPEG_AUDIO_L2_BITRATE``
    (enum)

enum v4l2_mpeg_audio_l2_bitrate -
    MPEG-1/2 Layer II bitrate. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_32K``
      - 32 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_48K``
      - 48 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_56K``
      - 56 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_64K``
      - 64 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_80K``
      - 80 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_96K``
      - 96 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_112K``
      - 112 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_128K``
      - 128 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_160K``
      - 160 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_192K``
      - 192 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_224K``
      - 224 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_256K``
      - 256 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_320K``
      - 320 kbit/s
    * - ``V4L2_MPEG_AUDIO_L2_BITRATE_384K``
      - 384 kbit/s



.. _v4l2-mpeg-audio-l3-bitrate:

``V4L2_CID_MPEG_AUDIO_L3_BITRATE``
    (enum)

enum v4l2_mpeg_audio_l3_bitrate -
    MPEG-1/2 Layer III bitrate. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_32K``
      - 32 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_40K``
      - 40 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_48K``
      - 48 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_56K``
      - 56 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_64K``
      - 64 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_80K``
      - 80 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_96K``
      - 96 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_112K``
      - 112 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_128K``
      - 128 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_160K``
      - 160 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_192K``
      - 192 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_224K``
      - 224 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_256K``
      - 256 kbit/s
    * - ``V4L2_MPEG_AUDIO_L3_BITRATE_320K``
      - 320 kbit/s



``V4L2_CID_MPEG_AUDIO_AAC_BITRATE (integer)``
    AAC bitrate in bits per second.

.. _v4l2-mpeg-audio-ac3-bitrate:

``V4L2_CID_MPEG_AUDIO_AC3_BITRATE``
    (enum)

enum v4l2_mpeg_audio_ac3_bitrate -
    AC-3 bitrate. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_32K``
      - 32 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_40K``
      - 40 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_48K``
      - 48 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_56K``
      - 56 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_64K``
      - 64 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_80K``
      - 80 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_96K``
      - 96 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_112K``
      - 112 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_128K``
      - 128 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_160K``
      - 160 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_192K``
      - 192 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_224K``
      - 224 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_256K``
      - 256 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_320K``
      - 320 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_384K``
      - 384 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_448K``
      - 448 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_512K``
      - 512 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_576K``
      - 576 kbit/s
    * - ``V4L2_MPEG_AUDIO_AC3_BITRATE_640K``
      - 640 kbit/s



.. _v4l2-mpeg-audio-mode:

``V4L2_CID_MPEG_AUDIO_MODE``
    (enum)

enum v4l2_mpeg_audio_mode -
    MPEG Audio mode. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_MODE_STEREO``
      - Stereo
    * - ``V4L2_MPEG_AUDIO_MODE_JOINT_STEREO``
      - Joint Stereo
    * - ``V4L2_MPEG_AUDIO_MODE_DUAL``
      - Bilingual
    * - ``V4L2_MPEG_AUDIO_MODE_MONO``
      - Mono



.. _v4l2-mpeg-audio-mode-extension:

``V4L2_CID_MPEG_AUDIO_MODE_EXTENSION``
    (enum)

enum v4l2_mpeg_audio_mode_extension -
    Joint Stereo audio mode extension. In Layer I and II they indicate
    which subbands are in intensity stereo. All other subbands are coded
    in stereo. Layer III is not (yet) supported. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_MODE_EXTENSION_BOUND_4``
      - Subbands 4-31 in intensity stereo
    * - ``V4L2_MPEG_AUDIO_MODE_EXTENSION_BOUND_8``
      - Subbands 8-31 in intensity stereo
    * - ``V4L2_MPEG_AUDIO_MODE_EXTENSION_BOUND_12``
      - Subbands 12-31 in intensity stereo
    * - ``V4L2_MPEG_AUDIO_MODE_EXTENSION_BOUND_16``
      - Subbands 16-31 in intensity stereo



.. _v4l2-mpeg-audio-emphasis:

``V4L2_CID_MPEG_AUDIO_EMPHASIS``
    (enum)

enum v4l2_mpeg_audio_emphasis -
    Audio Emphasis. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_EMPHASIS_NONE``
      - None
    * - ``V4L2_MPEG_AUDIO_EMPHASIS_50_DIV_15_uS``
      - 50/15 microsecond emphasis
    * - ``V4L2_MPEG_AUDIO_EMPHASIS_CCITT_J17``
      - CCITT J.17



.. _v4l2-mpeg-audio-crc:

``V4L2_CID_MPEG_AUDIO_CRC``
    (enum)

enum v4l2_mpeg_audio_crc -
    CRC method. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_CRC_NONE``
      - None
    * - ``V4L2_MPEG_AUDIO_CRC_CRC16``
      - 16 bit parity check



``V4L2_CID_MPEG_AUDIO_MUTE (boolean)``
    Mutes the audio when capturing. This is not done by muting audio
    hardware, which can still produce a slight hiss, but in the encoder
    itself, guaranteeing a fixed and reproducible audio bitstream. 0 =
    unmuted, 1 = muted.

.. _v4l2-mpeg-audio-dec-playback:

``V4L2_CID_MPEG_AUDIO_DEC_PLAYBACK``
    (enum)

enum v4l2_mpeg_audio_dec_playback -
    Determines how monolingual audio should be played back. Possible
    values are:



.. tabularcolumns:: |p{9.8cm}|p{7.7cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_AUDIO_DEC_PLAYBACK_AUTO``
      - Automatically determines the best playback mode.
    * - ``V4L2_MPEG_AUDIO_DEC_PLAYBACK_STEREO``
      - Stereo playback.
    * - ``V4L2_MPEG_AUDIO_DEC_PLAYBACK_LEFT``
      - Left channel playback.
    * - ``V4L2_MPEG_AUDIO_DEC_PLAYBACK_RIGHT``
      - Right channel playback.
    * - ``V4L2_MPEG_AUDIO_DEC_PLAYBACK_MONO``
      - Mono playback.
    * - ``V4L2_MPEG_AUDIO_DEC_PLAYBACK_SWAPPED_STEREO``
      - Stereo playback with swapped left and right channels.



.. _v4l2-mpeg-audio-dec-multilingual-playback:

``V4L2_CID_MPEG_AUDIO_DEC_MULTILINGUAL_PLAYBACK``
    (enum)

enum v4l2_mpeg_audio_dec_playback -
    Determines how multilingual audio should be played back.

.. _v4l2-mpeg-video-encoding:

``V4L2_CID_MPEG_VIDEO_ENCODING``
    (enum)

enum v4l2_mpeg_video_encoding -
    MPEG Video encoding method. This control is specific to multiplexed
    MPEG streams. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_ENCODING_MPEG_1``
      - MPEG-1 Video encoding
    * - ``V4L2_MPEG_VIDEO_ENCODING_MPEG_2``
      - MPEG-2 Video encoding
    * - ``V4L2_MPEG_VIDEO_ENCODING_MPEG_4_AVC``
      - MPEG-4 AVC (H.264) Video encoding



.. _v4l2-mpeg-video-aspect:

``V4L2_CID_MPEG_VIDEO_ASPECT``
    (enum)

enum v4l2_mpeg_video_aspect -
    Video aspect. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_ASPECT_1x1``
    * - ``V4L2_MPEG_VIDEO_ASPECT_4x3``
    * - ``V4L2_MPEG_VIDEO_ASPECT_16x9``
    * - ``V4L2_MPEG_VIDEO_ASPECT_221x100``



``V4L2_CID_MPEG_VIDEO_B_FRAMES (integer)``
    Number of B-Frames (default 2)

``V4L2_CID_MPEG_VIDEO_GOP_SIZE (integer)``
    GOP size (default 12)

``V4L2_CID_MPEG_VIDEO_GOP_CLOSURE (boolean)``
    GOP closure (default 1)

``V4L2_CID_MPEG_VIDEO_PULLDOWN (boolean)``
    Enable 3:2 pulldown (default 0)

.. _v4l2-mpeg-video-bitrate-mode:

``V4L2_CID_MPEG_VIDEO_BITRATE_MODE``
    (enum)

enum v4l2_mpeg_video_bitrate_mode -
    Video bitrate mode. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_BITRATE_MODE_VBR``
      - Variable bitrate
    * - ``V4L2_MPEG_VIDEO_BITRATE_MODE_CBR``
      - Constant bitrate



``V4L2_CID_MPEG_VIDEO_BITRATE (integer)``
    Video bitrate in bits per second.

``V4L2_CID_MPEG_VIDEO_BITRATE_PEAK (integer)``
    Peak video bitrate in bits per second. Must be larger or equal to
    the average video bitrate. It is ignored if the video bitrate mode
    is set to constant bitrate.

``V4L2_CID_MPEG_VIDEO_TEMPORAL_DECIMATION (integer)``
    For every captured frame, skip this many subsequent frames (default
    0).

``V4L2_CID_MPEG_VIDEO_MUTE (boolean)``
    "Mutes" the video to a fixed color when capturing. This is useful
    for testing, to produce a fixed video bitstream. 0 = unmuted, 1 =
    muted.

``V4L2_CID_MPEG_VIDEO_MUTE_YUV (integer)``
    Sets the "mute" color of the video. The supplied 32-bit integer is
    interpreted as follows (bit 0 = least significant bit):



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - Bit 0:7
      - V chrominance information
    * - Bit 8:15
      - U chrominance information
    * - Bit 16:23
      - Y luminance information
    * - Bit 24:31
      - Must be zero.



.. _v4l2-mpeg-video-dec-pts:

``V4L2_CID_MPEG_VIDEO_DEC_PTS (integer64)``
    This read-only control returns the 33-bit video Presentation Time
    Stamp as defined in ITU T-REC-H.222.0 and ISO/IEC 13818-1 of the
    currently displayed frame. This is the same PTS as is used in
    :ref:`VIDIOC_DECODER_CMD`.

.. _v4l2-mpeg-video-dec-frame:

``V4L2_CID_MPEG_VIDEO_DEC_FRAME (integer64)``
    This read-only control returns the frame counter of the frame that
    is currently displayed (decoded). This value is reset to 0 whenever
    the decoder is started.

``V4L2_CID_MPEG_VIDEO_DECODER_SLICE_INTERFACE (boolean)``
    If enabled the decoder expects to receive a single slice per buffer,
    otherwise the decoder expects a single frame in per buffer.
    Applicable to the decoder, all codecs.

``V4L2_CID_MPEG_VIDEO_H264_VUI_SAR_ENABLE (boolean)``
    Enable writing sample aspect ratio in the Video Usability
    Information. Applicable to the H264 encoder.

.. _v4l2-mpeg-video-h264-vui-sar-idc:

``V4L2_CID_MPEG_VIDEO_H264_VUI_SAR_IDC``
    (enum)

enum v4l2_mpeg_video_h264_vui_sar_idc -
    VUI sample aspect ratio indicator for H.264 encoding. The value is
    defined in the table E-1 in the standard. Applicable to the H264
    encoder.



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_UNSPECIFIED``
      - Unspecified
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_1x1``
      - 1x1
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_12x11``
      - 12x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_10x11``
      - 10x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_16x11``
      - 16x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_40x33``
      - 40x33
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_24x11``
      - 24x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_20x11``
      - 20x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_32x11``
      - 32x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_80x33``
      - 80x33
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_18x11``
      - 18x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_15x11``
      - 15x11
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_64x33``
      - 64x33
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_160x99``
      - 160x99
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_4x3``
      - 4x3
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_3x2``
      - 3x2
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_2x1``
      - 2x1
    * - ``V4L2_MPEG_VIDEO_H264_VUI_SAR_IDC_EXTENDED``
      - Extended SAR



``V4L2_CID_MPEG_VIDEO_H264_VUI_EXT_SAR_WIDTH (integer)``
    Extended sample aspect ratio width for H.264 VUI encoding.
    Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_VUI_EXT_SAR_HEIGHT (integer)``
    Extended sample aspect ratio height for H.264 VUI encoding.
    Applicable to the H264 encoder.

.. _v4l2-mpeg-video-h264-level:

``V4L2_CID_MPEG_VIDEO_H264_LEVEL``
    (enum)

enum v4l2_mpeg_video_h264_level -
    The level information for the H264 video elementary stream.
    Applicable to the H264 encoder. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_1_0``
      - Level 1.0
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_1B``
      - Level 1B
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_1_1``
      - Level 1.1
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_1_2``
      - Level 1.2
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_1_3``
      - Level 1.3
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_2_0``
      - Level 2.0
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_2_1``
      - Level 2.1
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_2_2``
      - Level 2.2
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_3_0``
      - Level 3.0
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_3_1``
      - Level 3.1
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_3_2``
      - Level 3.2
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_4_0``
      - Level 4.0
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_4_1``
      - Level 4.1
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_4_2``
      - Level 4.2
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_5_0``
      - Level 5.0
    * - ``V4L2_MPEG_VIDEO_H264_LEVEL_5_1``
      - Level 5.1



.. _v4l2-mpeg-video-mpeg2-level:

``V4L2_CID_MPEG_VIDEO_MPEG2_LEVEL``
    (enum)

enum v4l2_mpeg_video_mpeg2_level -
    The level information for the MPEG2 elementary stream. Applicable to
    MPEG2 codecs. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_MPEG2_LEVEL_LOW``
      - Low Level (LL)
    * - ``V4L2_MPEG_VIDEO_MPEG2_LEVEL_MAIN``
      - Main Level (ML)
    * - ``V4L2_MPEG_VIDEO_MPEG2_LEVEL_HIGH_1440``
      - High-1440 Level (H-14)
    * - ``V4L2_MPEG_VIDEO_MPEG2_LEVEL_HIGH``
      - High Level (HL)



.. _v4l2-mpeg-video-mpeg4-level:

``V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL``
    (enum)

enum v4l2_mpeg_video_mpeg4_level -
    The level information for the MPEG4 elementary stream. Applicable to
    the MPEG4 encoder. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_0``
      - Level 0
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_0B``
      - Level 0b
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_1``
      - Level 1
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_2``
      - Level 2
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_3``
      - Level 3
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_3B``
      - Level 3b
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_4``
      - Level 4
    * - ``V4L2_MPEG_VIDEO_MPEG4_LEVEL_5``
      - Level 5



.. _v4l2-mpeg-video-h264-profile:

``V4L2_CID_MPEG_VIDEO_H264_PROFILE``
    (enum)

enum v4l2_mpeg_video_h264_profile -
    The profile information for H264. Applicable to the H264 encoder.
    Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE``
      - Baseline profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE``
      - Constrained Baseline profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_MAIN``
      - Main profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED``
      - Extended profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH``
      - High profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10``
      - High 10 profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422``
      - High 422 profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE``
      - High 444 Predictive profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10_INTRA``
      - High 10 Intra profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422_INTRA``
      - High 422 Intra profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_INTRA``
      - High 444 Intra profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_CAVLC_444_INTRA``
      - CAVLC 444 Intra profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_BASELINE``
      - Scalable Baseline profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH``
      - Scalable High profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH_INTRA``
      - Scalable High Intra profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_STEREO_HIGH``
      - Stereo High profile
    * - ``V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH``
      - Multiview High profile



.. _v4l2-mpeg-video-mpeg2-profile:

``V4L2_CID_MPEG_VIDEO_MPEG2_PROFILE``
    (enum)

enum v4l2_mpeg_video_mpeg2_profile -
    The profile information for MPEG2. Applicable to MPEG2 codecs.
    Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_MPEG2_PROFILE_SIMPLE``
      - Simple profile (SP)
    * - ``V4L2_MPEG_VIDEO_MPEG2_PROFILE_MAIN``
      - Main profile (MP)
    * - ``V4L2_MPEG_VIDEO_MPEG2_PROFILE_SNR_SCALABLE``
      - SNR Scalable profile (SNR)
    * - ``V4L2_MPEG_VIDEO_MPEG2_PROFILE_SPATIALLY_SCALABLE``
      - Spatially Scalable profile (Spt)
    * - ``V4L2_MPEG_VIDEO_MPEG2_PROFILE_HIGH``
      - High profile (HP)
    * - ``V4L2_MPEG_VIDEO_MPEG2_PROFILE_MULTIVIEW``
      - Multi-view profile (MVP)



.. _v4l2-mpeg-video-mpeg4-profile:

``V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE``
    (enum)

enum v4l2_mpeg_video_mpeg4_profile -
    The profile information for MPEG4. Applicable to the MPEG4 encoder.
    Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_MPEG4_PROFILE_SIMPLE``
      - Simple profile
    * - ``V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_SIMPLE``
      - Advanced Simple profile
    * - ``V4L2_MPEG_VIDEO_MPEG4_PROFILE_CORE``
      - Core profile
    * - ``V4L2_MPEG_VIDEO_MPEG4_PROFILE_SIMPLE_SCALABLE``
      - Simple Scalable profile
    * - ``V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_CODING_EFFICIENCY``
      -



``V4L2_CID_MPEG_VIDEO_MAX_REF_PIC (integer)``
    The maximum number of reference pictures used for encoding.
    Applicable to the encoder.

.. _v4l2-mpeg-video-multi-slice-mode:

``V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE``
    (enum)

enum v4l2_mpeg_video_multi_slice_mode -
    Determines how the encoder should handle division of frame into
    slices. Applicable to the encoder. Possible values are:



.. tabularcolumns:: |p{9.6cm}|p{7.9cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_SINGLE``
      - Single slice per frame.
    * - ``V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_MAX_MB``
      - Multiple slices with set maximum number of macroblocks per slice.
    * - ``V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_MAX_BYTES``
      - Multiple slice with set maximum size in bytes per slice.



``V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB (integer)``
    The maximum number of macroblocks in a slice. Used when
    ``V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE`` is set to
    ``V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_MAX_MB``. Applicable to the
    encoder.

``V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_BYTES (integer)``
    The maximum size of a slice in bytes. Used when
    ``V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE`` is set to
    ``V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_MAX_BYTES``. Applicable to the
    encoder.

.. _v4l2-mpeg-video-h264-loop-filter-mode:

``V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE``
    (enum)

enum v4l2_mpeg_video_h264_loop_filter_mode -
    Loop filter mode for H264 encoder. Possible values are:

.. raw:: latex

    \small

.. tabularcolumns:: |p{13.6cm}|p{3.9cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_ENABLED``
      - Loop filter is enabled.
    * - ``V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED``
      - Loop filter is disabled.
    * - ``V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY``
      - Loop filter is disabled at the slice boundary.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA (integer)``
    Loop filter alpha coefficient, defined in the H264 standard.
    This value corresponds to the slice_alpha_c0_offset_div2 slice header
    field, and should be in the range of -6 to +6, inclusive. The actual alpha
    offset FilterOffsetA is twice this value.
    Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA (integer)``
    Loop filter beta coefficient, defined in the H264 standard.
    This corresponds to the slice_beta_offset_div2 slice header field, and
    should be in the range of -6 to +6, inclusive. The actual beta offset
    FilterOffsetB is twice this value.
    Applicable to the H264 encoder.

.. _v4l2-mpeg-video-h264-entropy-mode:

``V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE``
    (enum)

enum v4l2_mpeg_video_h264_entropy_mode -
    Entropy coding mode for H264 - CABAC/CAVALC. Applicable to the H264
    encoder. Possible values are:


.. tabularcolumns:: |p{9.0cm}|p{8.5cm}|


.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC``
      - Use CAVLC entropy coding.
    * - ``V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC``
      - Use CABAC entropy coding.



``V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM (boolean)``
    Enable 8X8 transform for H264. Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_CONSTRAINED_INTRA_PREDICTION (boolean)``
    Enable constrained intra prediction for H264. Applicable to the H264
    encoder.

``V4L2_CID_MPEG_VIDEO_H264_CHROMA_QP_INDEX_OFFSET (integer)``
    Specify the offset that should be added to the luma quantization
    parameter to determine the chroma quantization parameter. Applicable
    to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_CYCLIC_INTRA_REFRESH_MB (integer)``
    Cyclic intra macroblock refresh. This is the number of continuous
    macroblocks refreshed every frame. Each frame a successive set of
    macroblocks is refreshed until the cycle completes and starts from
    the top of the frame. Applicable to H264, H263 and MPEG4 encoder.

``V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE (boolean)``
    Frame level rate control enable. If this control is disabled then
    the quantization parameter for each frame type is constant and set
    with appropriate controls (e.g.
    ``V4L2_CID_MPEG_VIDEO_H263_I_FRAME_QP``). If frame rate control is
    enabled then quantization parameter is adjusted to meet the chosen
    bitrate. Minimum and maximum value for the quantization parameter
    can be set with appropriate controls (e.g.
    ``V4L2_CID_MPEG_VIDEO_H263_MIN_QP``). Applicable to encoders.

``V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE (boolean)``
    Macroblock level rate control enable. Applicable to the MPEG4 and
    H264 encoders.

``V4L2_CID_MPEG_VIDEO_MPEG4_QPEL (boolean)``
    Quarter pixel motion estimation for MPEG4. Applicable to the MPEG4
    encoder.

``V4L2_CID_MPEG_VIDEO_H263_I_FRAME_QP (integer)``
    Quantization parameter for an I frame for H263. Valid range: from 1
    to 31.

``V4L2_CID_MPEG_VIDEO_H263_MIN_QP (integer)``
    Minimum quantization parameter for H263. Valid range: from 1 to 31.

``V4L2_CID_MPEG_VIDEO_H263_MAX_QP (integer)``
    Maximum quantization parameter for H263. Valid range: from 1 to 31.

``V4L2_CID_MPEG_VIDEO_H263_P_FRAME_QP (integer)``
    Quantization parameter for an P frame for H263. Valid range: from 1
    to 31.

``V4L2_CID_MPEG_VIDEO_H263_B_FRAME_QP (integer)``
    Quantization parameter for an B frame for H263. Valid range: from 1
    to 31.

``V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP (integer)``
    Quantization parameter for an I frame for H264. Valid range: from 0
    to 51.

``V4L2_CID_MPEG_VIDEO_H264_MIN_QP (integer)``
    Minimum quantization parameter for H264. Valid range: from 0 to 51.

``V4L2_CID_MPEG_VIDEO_H264_MAX_QP (integer)``
    Maximum quantization parameter for H264. Valid range: from 0 to 51.

``V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP (integer)``
    Quantization parameter for an P frame for H264. Valid range: from 0
    to 51.

``V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP (integer)``
    Quantization parameter for an B frame for H264. Valid range: from 0
    to 51.

``V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MIN_QP (integer)``
    Minimum quantization parameter for the H264 I frame to limit I frame
    quality to a range. Valid range: from 0 to 51. If
    V4L2_CID_MPEG_VIDEO_H264_MIN_QP is also set, the quantization parameter
    should be chosen to meet both requirements.

``V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MAX_QP (integer)``
    Maximum quantization parameter for the H264 I frame to limit I frame
    quality to a range. Valid range: from 0 to 51. If
    V4L2_CID_MPEG_VIDEO_H264_MAX_QP is also set, the quantization parameter
    should be chosen to meet both requirements.

``V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MIN_QP (integer)``
    Minimum quantization parameter for the H264 P frame to limit P frame
    quality to a range. Valid range: from 0 to 51. If
    V4L2_CID_MPEG_VIDEO_H264_MIN_QP is also set, the quantization parameter
    should be chosen to meet both requirements.

``V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MAX_QP (integer)``
    Maximum quantization parameter for the H264 P frame to limit P frame
    quality to a range. Valid range: from 0 to 51. If
    V4L2_CID_MPEG_VIDEO_H264_MAX_QP is also set, the quantization parameter
    should be chosen to meet both requirements.

``V4L2_CID_MPEG_VIDEO_MPEG4_I_FRAME_QP (integer)``
    Quantization parameter for an I frame for MPEG4. Valid range: from 1
    to 31.

``V4L2_CID_MPEG_VIDEO_MPEG4_MIN_QP (integer)``
    Minimum quantization parameter for MPEG4. Valid range: from 1 to 31.

``V4L2_CID_MPEG_VIDEO_MPEG4_MAX_QP (integer)``
    Maximum quantization parameter for MPEG4. Valid range: from 1 to 31.

``V4L2_CID_MPEG_VIDEO_MPEG4_P_FRAME_QP (integer)``
    Quantization parameter for an P frame for MPEG4. Valid range: from 1
    to 31.

``V4L2_CID_MPEG_VIDEO_MPEG4_B_FRAME_QP (integer)``
    Quantization parameter for an B frame for MPEG4. Valid range: from 1
    to 31.

``V4L2_CID_MPEG_VIDEO_VBV_SIZE (integer)``
    The Video Buffer Verifier size in kilobytes, it is used as a
    limitation of frame skip. The VBV is defined in the standard as a
    mean to verify that the produced stream will be successfully
    decoded. The standard describes it as "Part of a hypothetical
    decoder that is conceptually connected to the output of the encoder.
    Its purpose is to provide a constraint on the variability of the
    data rate that an encoder or editing process may produce.".
    Applicable to the MPEG1, MPEG2, MPEG4 encoders.

.. _v4l2-mpeg-video-vbv-delay:

``V4L2_CID_MPEG_VIDEO_VBV_DELAY (integer)``
    Sets the initial delay in milliseconds for VBV buffer control.

.. _v4l2-mpeg-video-hor-search-range:

``V4L2_CID_MPEG_VIDEO_MV_H_SEARCH_RANGE (integer)``
    Horizontal search range defines maximum horizontal search area in
    pixels to search and match for the present Macroblock (MB) in the
    reference picture. This V4L2 control macro is used to set horizontal
    search range for motion estimation module in video encoder.

.. _v4l2-mpeg-video-vert-search-range:

``V4L2_CID_MPEG_VIDEO_MV_V_SEARCH_RANGE (integer)``
    Vertical search range defines maximum vertical search area in pixels
    to search and match for the present Macroblock (MB) in the reference
    picture. This V4L2 control macro is used to set vertical search
    range for motion estimation module in video encoder.

.. _v4l2-mpeg-video-force-key-frame:

``V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME (button)``
    Force a key frame for the next queued buffer. Applicable to
    encoders. This is a general, codec-agnostic keyframe control.

``V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE (integer)``
    The Coded Picture Buffer size in kilobytes, it is used as a
    limitation of frame skip. The CPB is defined in the H264 standard as
    a mean to verify that the produced stream will be successfully
    decoded. Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_I_PERIOD (integer)``
    Period between I-frames in the open GOP for H264. In case of an open
    GOP this is the period between two I-frames. The period between IDR
    (Instantaneous Decoding Refresh) frames is taken from the GOP_SIZE
    control. An IDR frame, which stands for Instantaneous Decoding
    Refresh is an I-frame after which no prior frames are referenced.
    This means that a stream can be restarted from an IDR frame without
    the need to store or decode any previous frames. Applicable to the
    H264 encoder.

.. _v4l2-mpeg-video-header-mode:

``V4L2_CID_MPEG_VIDEO_HEADER_MODE``
    (enum)

enum v4l2_mpeg_video_header_mode -
    Determines whether the header is returned as the first buffer or is
    it returned together with the first frame. Applicable to encoders.
    Possible values are:

.. raw:: latex

    \small

.. tabularcolumns:: |p{10.3cm}|p{7.2cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE``
      - The stream header is returned separately in the first buffer.
    * - ``V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME``
      - The stream header is returned together with the first encoded
	frame.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER (boolean)``
    Repeat the video sequence headers. Repeating these headers makes
    random access to the video stream easier. Applicable to the MPEG1, 2
    and 4 encoder.

``V4L2_CID_MPEG_VIDEO_DECODER_MPEG4_DEBLOCK_FILTER (boolean)``
    Enabled the deblocking post processing filter for MPEG4 decoder.
    Applicable to the MPEG4 decoder.

``V4L2_CID_MPEG_VIDEO_MPEG4_VOP_TIME_RES (integer)``
    vop_time_increment_resolution value for MPEG4. Applicable to the
    MPEG4 encoder.

``V4L2_CID_MPEG_VIDEO_MPEG4_VOP_TIME_INC (integer)``
    vop_time_increment value for MPEG4. Applicable to the MPEG4
    encoder.

``V4L2_CID_MPEG_VIDEO_H264_SEI_FRAME_PACKING (boolean)``
    Enable generation of frame packing supplemental enhancement
    information in the encoded bitstream. The frame packing SEI message
    contains the arrangement of L and R planes for 3D viewing.
    Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_SEI_FP_CURRENT_FRAME_0 (boolean)``
    Sets current frame as frame0 in frame packing SEI. Applicable to the
    H264 encoder.

.. _v4l2-mpeg-video-h264-sei-fp-arrangement-type:

``V4L2_CID_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE``
    (enum)

enum v4l2_mpeg_video_h264_sei_fp_arrangement_type -
    Frame packing arrangement type for H264 SEI. Applicable to the H264
    encoder. Possible values are:

.. raw:: latex

    \small

.. tabularcolumns:: |p{12cm}|p{5.5cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE_CHEKERBOARD``
      - Pixels are alternatively from L and R.
    * - ``V4L2_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE_COLUMN``
      - L and R are interlaced by column.
    * - ``V4L2_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE_ROW``
      - L and R are interlaced by row.
    * - ``V4L2_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE_SIDE_BY_SIDE``
      - L is on the left, R on the right.
    * - ``V4L2_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE_TOP_BOTTOM``
      - L is on top, R on bottom.
    * - ``V4L2_MPEG_VIDEO_H264_SEI_FP_ARRANGEMENT_TYPE_TEMPORAL``
      - One view per frame.

.. raw:: latex

    \normalsize



``V4L2_CID_MPEG_VIDEO_H264_FMO (boolean)``
    Enables flexible macroblock ordering in the encoded bitstream. It is
    a technique used for restructuring the ordering of macroblocks in
    pictures. Applicable to the H264 encoder.

.. _v4l2-mpeg-video-h264-fmo-map-type:

``V4L2_CID_MPEG_VIDEO_H264_FMO_MAP_TYPE``
   (enum)

enum v4l2_mpeg_video_h264_fmo_map_type -
    When using FMO, the map type divides the image in different scan
    patterns of macroblocks. Applicable to the H264 encoder. Possible
    values are:

.. raw:: latex

    \small

.. tabularcolumns:: |p{12.5cm}|p{5.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_INTERLEAVED_SLICES``
      - Slices are interleaved one after other with macroblocks in run
	length order.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_SCATTERED_SLICES``
      - Scatters the macroblocks based on a mathematical function known to
	both encoder and decoder.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_FOREGROUND_WITH_LEFT_OVER``
      - Macroblocks arranged in rectangular areas or regions of interest.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_BOX_OUT``
      - Slice groups grow in a cyclic way from centre to outwards.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_RASTER_SCAN``
      - Slice groups grow in raster scan pattern from left to right.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_WIPE_SCAN``
      - Slice groups grow in wipe scan pattern from top to bottom.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_MAP_TYPE_EXPLICIT``
      - User defined map type.

.. raw:: latex

    \normalsize



``V4L2_CID_MPEG_VIDEO_H264_FMO_SLICE_GROUP (integer)``
    Number of slice groups in FMO. Applicable to the H264 encoder.

.. _v4l2-mpeg-video-h264-fmo-change-direction:

``V4L2_CID_MPEG_VIDEO_H264_FMO_CHANGE_DIRECTION``
    (enum)

enum v4l2_mpeg_video_h264_fmo_change_dir -
    Specifies a direction of the slice group change for raster and wipe
    maps. Applicable to the H264 encoder. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_FMO_CHANGE_DIR_RIGHT``
      - Raster scan or wipe right.
    * - ``V4L2_MPEG_VIDEO_H264_FMO_CHANGE_DIR_LEFT``
      - Reverse raster scan or wipe left.



``V4L2_CID_MPEG_VIDEO_H264_FMO_CHANGE_RATE (integer)``
    Specifies the size of the first slice group for raster and wipe map.
    Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_FMO_RUN_LENGTH (integer)``
    Specifies the number of consecutive macroblocks for the interleaved
    map. Applicable to the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_ASO (boolean)``
    Enables arbitrary slice ordering in encoded bitstream. Applicable to
    the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_ASO_SLICE_ORDER (integer)``
    Specifies the slice order in ASO. Applicable to the H264 encoder.
    The supplied 32-bit integer is interpreted as follows (bit 0 = least
    significant bit):



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - Bit 0:15
      - Slice ID
    * - Bit 16:32
      - Slice position or order



``V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING (boolean)``
    Enables H264 hierarchical coding. Applicable to the H264 encoder.

.. _v4l2-mpeg-video-h264-hierarchical-coding-type:

``V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_TYPE``
    (enum)

enum v4l2_mpeg_video_h264_hierarchical_coding_type -
    Specifies the hierarchical coding type. Applicable to the H264
    encoder. Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_H264_HIERARCHICAL_CODING_B``
      - Hierarchical B coding.
    * - ``V4L2_MPEG_VIDEO_H264_HIERARCHICAL_CODING_P``
      - Hierarchical P coding.



``V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER (integer)``
    Specifies the number of hierarchical coding layers. Applicable to
    the H264 encoder.

``V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER_QP (integer)``
    Specifies a user defined QP for each layer. Applicable to the H264
    encoder. The supplied 32-bit integer is interpreted as follows (bit
    0 = least significant bit):



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - Bit 0:15
      - QP value
    * - Bit 16:32
      - Layer number


.. _v4l2-mpeg-h264:

``V4L2_CID_MPEG_VIDEO_H264_SPS (struct)``
    Specifies the sequence parameter set (as extracted from the
    bitstream) for the associated H264 slice data. This includes the
    necessary parameters for configuring a stateless hardware decoding
    pipeline for H264. The bitstream parameters are defined according
    to :ref:`h264`, section 7.4.2.1.1 "Sequence Parameter Set Data
    Semantics". For further documentation, refer to the above
    specification, unless there is an explicit comment stating
    otherwise.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_h264_sps

.. cssclass:: longtable

.. flat-table:: struct v4l2_ctrl_h264_sps
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``profile_idc``
      -
    * - __u8
      - ``constraint_set_flags``
      - See :ref:`Sequence Parameter Set Constraints Set Flags <h264_sps_constraints_set_flags>`
    * - __u8
      - ``level_idc``
      -
    * - __u8
      - ``seq_parameter_set_id``
      -
    * - __u8
      - ``chroma_format_idc``
      -
    * - __u8
      - ``bit_depth_luma_minus8``
      -
    * - __u8
      - ``bit_depth_chroma_minus8``
      -
    * - __u8
      - ``log2_max_frame_num_minus4``
      -
    * - __u8
      - ``pic_order_cnt_type``
      -
    * - __u8
      - ``log2_max_pic_order_cnt_lsb_minus4``
      -
    * - __u8
      - ``max_num_ref_frames``
      -
    * - __u8
      - ``num_ref_frames_in_pic_order_cnt_cycle``
      -
    * - __s32
      - ``offset_for_ref_frame[255]``
      -
    * - __s32
      - ``offset_for_non_ref_pic``
      -
    * - __s32
      - ``offset_for_top_to_bottom_field``
      -
    * - __u16
      - ``pic_width_in_mbs_minus1``
      -
    * - __u16
      - ``pic_height_in_map_units_minus1``
      -
    * - __u32
      - ``flags``
      - See :ref:`Sequence Parameter Set Flags <h264_sps_flags>`

.. _h264_sps_constraints_set_flags:

``Sequence Parameter Set Constraints Set Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_H264_SPS_CONSTRAINT_SET0_FLAG``
      - 0x00000001
      -
    * - ``V4L2_H264_SPS_CONSTRAINT_SET1_FLAG``
      - 0x00000002
      -
    * - ``V4L2_H264_SPS_CONSTRAINT_SET2_FLAG``
      - 0x00000004
      -
    * - ``V4L2_H264_SPS_CONSTRAINT_SET3_FLAG``
      - 0x00000008
      -
    * - ``V4L2_H264_SPS_CONSTRAINT_SET4_FLAG``
      - 0x00000010
      -
    * - ``V4L2_H264_SPS_CONSTRAINT_SET5_FLAG``
      - 0x00000020
      -

.. _h264_sps_flags:

``Sequence Parameter Set Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE``
      - 0x00000001
      -
    * - ``V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS``
      - 0x00000002
      -
    * - ``V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO``
      - 0x00000004
      -
    * - ``V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED``
      - 0x00000008
      -
    * - ``V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY``
      - 0x00000010
      -
    * - ``V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD``
      - 0x00000020
      -
    * - ``V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE``
      - 0x00000040
      -

``V4L2_CID_MPEG_VIDEO_H264_PPS (struct)``
    Specifies the picture parameter set (as extracted from the
    bitstream) for the associated H264 slice data. This includes the
    necessary parameters for configuring a stateless hardware decoding
    pipeline for H264.  The bitstream parameters are defined according
    to :ref:`h264`, section 7.4.2.2 "Picture Parameter Set RBSP
    Semantics". For further documentation, refer to the above
    specification, unless there is an explicit comment stating
    otherwise.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_h264_pps

.. cssclass:: longtable

.. flat-table:: struct v4l2_ctrl_h264_pps
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``pic_parameter_set_id``
      -
    * - __u8
      - ``seq_parameter_set_id``
      -
    * - __u8
      - ``num_slice_groups_minus1``
      -
    * - __u8
      - ``num_ref_idx_l0_default_active_minus1``
      -
    * - __u8
      - ``num_ref_idx_l1_default_active_minus1``
      -
    * - __u8
      - ``weighted_bipred_idc``
      -
    * - __s8
      - ``pic_init_qp_minus26``
      -
    * - __s8
      - ``pic_init_qs_minus26``
      -
    * - __s8
      - ``chroma_qp_index_offset``
      -
    * - __s8
      - ``second_chroma_qp_index_offset``
      -
    * - __u16
      - ``flags``
      - See :ref:`Picture Parameter Set Flags <h264_pps_flags>`

.. _h264_pps_flags:

``Picture Parameter Set Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE``
      - 0x00000001
      -
    * - ``V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT``
      - 0x00000002
      -
    * - ``V4L2_H264_PPS_FLAG_WEIGHTED_PRED``
      - 0x00000004
      -
    * - ``V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT``
      - 0x00000008
      -
    * - ``V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED``
      - 0x00000010
      -
    * - ``V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT``
      - 0x00000020
      -
    * - ``V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE``
      - 0x00000040
      -
    * - ``V4L2_H264_PPS_FLAG_PIC_SCALING_MATRIX_PRESENT``
      - 0x00000080
      -

``V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX (struct)``
    Specifies the scaling matrix (as extracted from the bitstream) for
    the associated H264 slice data. The bitstream parameters are
    defined according to :ref:`h264`, section 7.4.2.1.1.1 "Scaling
    List Semantics". For further documentation, refer to the above
    specification, unless there is an explicit comment stating
    otherwise.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_h264_scaling_matrix

.. cssclass:: longtable

.. flat-table:: struct v4l2_ctrl_h264_scaling_matrix
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``scaling_list_4x4[6][16]``
      -
    * - __u8
      - ``scaling_list_8x8[6][64]``
      -

``V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAMS (struct)``
    Specifies the slice parameters (as extracted from the bitstream)
    for the associated H264 slice data. This includes the necessary
    parameters for configuring a stateless hardware decoding pipeline
    for H264.  The bitstream parameters are defined according to
    :ref:`h264`, section 7.4.3 "Slice Header Semantics". For further
    documentation, refer to the above specification, unless there is
    an explicit comment stating otherwise.

    .. note::

       This compound control is not yet part of the public kernel API
       and it is expected to change.

       This structure is expected to be passed as an array, with one
       entry for each slice included in the bitstream buffer.

.. c:type:: v4l2_ctrl_h264_slice_params

.. cssclass:: longtable

.. flat-table:: struct v4l2_ctrl_h264_slice_params
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``size``
      -
    * - __u32
      - ``start_byte_offset``
        Offset (in bytes) from the beginning of the OUTPUT buffer to the start
        of the slice. If the slice starts with a start code, then this is the
        offset to such start code. When operating in slice-based decoding mode
        (see :c:type:`v4l2_mpeg_video_h264_decode_mode`), this field should
        be set to 0. When operating in frame-based decoding mode, this field
        should be 0 for the first slice.
    * - __u32
      - ``header_bit_size``
      -
    * - __u16
      - ``first_mb_in_slice``
      -
    * - __u8
      - ``slice_type``
      -
    * - __u8
      - ``pic_parameter_set_id``
      -
    * - __u8
      - ``colour_plane_id``
      -
    * - __u8
      - ``redundant_pic_cnt``
      -
    * - __u16
      - ``frame_num``
      -
    * - __u16
      - ``idr_pic_id``
      -
    * - __u16
      - ``pic_order_cnt_lsb``
      -
    * - __s32
      - ``delta_pic_order_cnt_bottom``
      -
    * - __s32
      - ``delta_pic_order_cnt0``
      -
    * - __s32
      - ``delta_pic_order_cnt1``
      -
    * - struct :c:type:`v4l2_h264_pred_weight_table`
      - ``pred_weight_table``
      -
    * - __u32
      - ``dec_ref_pic_marking_bit_size``
      -
    * - __u32
      - ``pic_order_cnt_bit_size``
      -
    * - __u8
      - ``cabac_init_idc``
      -
    * - __s8
      - ``slice_qp_delta``
      -
    * - __s8
      - ``slice_qs_delta``
      -
    * - __u8
      - ``disable_deblocking_filter_idc``
      -
    * - __s8
      - ``slice_alpha_c0_offset_div2``
      -
    * - __s8
      - ``slice_beta_offset_div2``
      -
    * - __u8
      - ``num_ref_idx_l0_active_minus1``
      -
    * - __u8
      - ``num_ref_idx_l1_active_minus1``
      -
    * - __u32
      - ``slice_group_change_cycle``
      -
    * - __u8
      - ``ref_pic_list0[32]``
      - Reference picture list after applying the per-slice modifications
    * - __u8
      - ``ref_pic_list1[32]``
      - Reference picture list after applying the per-slice modifications
    * - __u32
      - ``flags``
      - See :ref:`Slice Parameter Flags <h264_slice_flags>`

.. _h264_slice_flags:

``Slice Parameter Set Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_H264_SLICE_FLAG_FIELD_PIC``
      - 0x00000001
      -
    * - ``V4L2_H264_SLICE_FLAG_BOTTOM_FIELD``
      - 0x00000002
      -
    * - ``V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED``
      - 0x00000004
      -
    * - ``V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH``
      - 0x00000008
      -

``Prediction Weight Table``

    The bitstream parameters are defined according to :ref:`h264`,
    section 7.4.3.2 "Prediction Weight Table Semantics". For further
    documentation, refer to the above specification, unless there is
    an explicit comment stating otherwise.

.. c:type:: v4l2_h264_pred_weight_table

.. cssclass:: longtable

.. flat-table:: struct v4l2_h264_pred_weight_table
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u16
      - ``luma_log2_weight_denom``
      -
    * - __u16
      - ``chroma_log2_weight_denom``
      -
    * - struct :c:type:`v4l2_h264_weight_factors`
      - ``weight_factors[2]``
      - The weight factors at index 0 are the weight factors for the reference
        list 0, the one at index 1 for the reference list 1.

.. c:type:: v4l2_h264_weight_factors

.. cssclass:: longtable

.. flat-table:: struct v4l2_h264_weight_factors
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __s16
      - ``luma_weight[32]``
      -
    * - __s16
      - ``luma_offset[32]``
      -
    * - __s16
      - ``chroma_weight[32][2]``
      -
    * - __s16
      - ``chroma_offset[32][2]``
      -

``V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAMS (struct)``
    Specifies the decode parameters (as extracted from the bitstream)
    for the associated H264 slice data. This includes the necessary
    parameters for configuring a stateless hardware decoding pipeline
    for H264. The bitstream parameters are defined according to
    :ref:`h264`. For further documentation, refer to the above
    specification, unless there is an explicit comment stating
    otherwise.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_h264_decode_params

.. cssclass:: longtable

.. flat-table:: struct v4l2_ctrl_h264_decode_params
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - struct :c:type:`v4l2_h264_dpb_entry`
      - ``dpb[16]``
      -
    * - __u16
      - ``num_slices``
      - Number of slices needed to decode the current frame/field. When
        operating in slice-based decoding mode (see
        :c:type:`v4l2_mpeg_video_h264_decode_mode`), this field
        should always be set to one.
    * - __u16
      - ``nal_ref_idc``
      - NAL reference ID value coming from the NAL Unit header
    * - __s32
      - ``top_field_order_cnt``
      - Picture Order Count for the coded top field
    * - __s32
      - ``bottom_field_order_cnt``
      - Picture Order Count for the coded bottom field
    * - __u32
      - ``flags``
      - See :ref:`Decode Parameters Flags <h264_decode_params_flags>`

.. _h264_decode_params_flags:

``Decode Parameters Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC``
      - 0x00000001
      - That picture is an IDR picture

.. c:type:: v4l2_h264_dpb_entry

.. cssclass:: longtable

.. flat-table:: struct v4l2_h264_dpb_entry
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u64
      - ``reference_ts``
      - Timestamp of the V4L2 capture buffer to use as reference, used
        with B-coded and P-coded frames. The timestamp refers to the
	``timestamp`` field in struct :c:type:`v4l2_buffer`. Use the
	:c:func:`v4l2_timeval_to_ns()` function to convert the struct
	:c:type:`timeval` in struct :c:type:`v4l2_buffer` to a __u64.
    * - __u16
      - ``frame_num``
      -
    * - __u16
      - ``pic_num``
      -
    * - __s32
      - ``top_field_order_cnt``
      -
    * - __s32
      - ``bottom_field_order_cnt``
      -
    * - __u32
      - ``flags``
      - See :ref:`DPB Entry Flags <h264_dpb_flags>`

.. _h264_dpb_flags:

``DPB Entries Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_H264_DPB_ENTRY_FLAG_VALID``
      - 0x00000001
      - The DPB entry is valid and should be considered
    * - ``V4L2_H264_DPB_ENTRY_FLAG_ACTIVE``
      - 0x00000002
      - The DPB entry is currently being used as a reference frame
    * - ``V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM``
      - 0x00000004
      - The DPB entry is a long term reference frame

``V4L2_CID_MPEG_VIDEO_H264_DECODE_MODE (enum)``
    Specifies the decoding mode to use. Currently exposes slice-based and
    frame-based decoding but new modes might be added later on.
    This control is used as a modifier for V4L2_PIX_FMT_H264_SLICE
    pixel format. Applications that support V4L2_PIX_FMT_H264_SLICE
    are required to set this control in order to specify the decoding mode
    that is expected for the buffer.
    Drivers may expose a single or multiple decoding modes, depending
    on what they can support.

    .. note::

       This menu control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_mpeg_video_h264_decode_mode

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_MPEG_VIDEO_H264_DECODE_MODE_SLICE_BASED``
      - 0
      - Decoding is done at the slice granularity.
        In this mode, ``num_slices`` field in struct
        :c:type:`v4l2_ctrl_h264_decode_params` should be set to 1,
        and ``start_byte_offset`` in struct
        :c:type:`v4l2_ctrl_h264_slice_params` should be set to 0.
        The OUTPUT buffer must contain a single slice.
    * - ``V4L2_MPEG_VIDEO_H264_DECODE_MODE_FRAME_BASED``
      - 1
      - Decoding is done at the frame granularity.
        In this mode, ``num_slices`` field in struct
        :c:type:`v4l2_ctrl_h264_decode_params` should be set to the number
        of slices in the frame, and ``start_byte_offset`` in struct
        :c:type:`v4l2_ctrl_h264_slice_params` should be set accordingly
        for each slice. For the first slice, ``start_byte_offset`` should
        be zero.
        The OUTPUT buffer must contain all slices needed to decode the
        frame. The OUTPUT buffer must also contain both fields.

``V4L2_CID_MPEG_VIDEO_H264_START_CODE (enum)``
    Specifies the H264 slice start code expected for each slice.
    This control is used as a modifier for V4L2_PIX_FMT_H264_SLICE
    pixel format. Applications that support V4L2_PIX_FMT_H264_SLICE
    are required to set this control in order to specify the start code
    that is expected for the buffer.
    Drivers may expose a single or multiple start codes, depending
    on what they can support.

    .. note::

       This menu control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_mpeg_video_h264_start_code

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_MPEG_VIDEO_H264_START_CODE_NONE``
      - 0
      - Selecting this value specifies that H264 slices are passed
        to the driver without any start code.
    * - ``V4L2_MPEG_VIDEO_H264_START_CODE_ANNEX_B``
      - 1
      - Selecting this value specifies that H264 slices are expected
        to be prefixed by Annex B start codes. According to :ref:`h264`
        valid start codes can be 3-bytes 0x000001 or 4-bytes 0x00000001.

.. _v4l2-mpeg-mpeg2:

``V4L2_CID_MPEG_VIDEO_MPEG2_SLICE_PARAMS (struct)``
    Specifies the slice parameters (as extracted from the bitstream) for the
    associated MPEG-2 slice data. This includes the necessary parameters for
    configuring a stateless hardware decoding pipeline for MPEG-2.
    The bitstream parameters are defined according to :ref:`mpeg2part2`.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_mpeg2_slice_params

.. cssclass:: longtable

.. tabularcolumns:: |p{5.8cm}|p{4.8cm}|p{6.6cm}|

.. flat-table:: struct v4l2_ctrl_mpeg2_slice_params
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``bit_size``
      - Size (in bits) of the current slice data.
    * - __u32
      - ``data_bit_offset``
      - Offset (in bits) to the video data in the current slice data.
    * - struct :c:type:`v4l2_mpeg2_sequence`
      - ``sequence``
      - Structure with MPEG-2 sequence metadata, merging relevant fields from
	the sequence header and sequence extension parts of the bitstream.
    * - struct :c:type:`v4l2_mpeg2_picture`
      - ``picture``
      - Structure with MPEG-2 picture metadata, merging relevant fields from
	the picture header and picture coding extension parts of the bitstream.
    * - __u64
      - ``backward_ref_ts``
      - Timestamp of the V4L2 capture buffer to use as backward reference, used
        with B-coded and P-coded frames. The timestamp refers to the
	``timestamp`` field in struct :c:type:`v4l2_buffer`. Use the
	:c:func:`v4l2_timeval_to_ns()` function to convert the struct
	:c:type:`timeval` in struct :c:type:`v4l2_buffer` to a __u64.
    * - __u64
      - ``forward_ref_ts``
      - Timestamp for the V4L2 capture buffer to use as forward reference, used
        with B-coded frames. The timestamp refers to the ``timestamp`` field in
	struct :c:type:`v4l2_buffer`. Use the :c:func:`v4l2_timeval_to_ns()`
	function to convert the struct :c:type:`timeval` in struct
	:c:type:`v4l2_buffer` to a __u64.
    * - __u32
      - ``quantiser_scale_code``
      - Code used to determine the quantization scale to use for the IDCT.

.. c:type:: v4l2_mpeg2_sequence

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_mpeg2_sequence
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u16
      - ``horizontal_size``
      - The width of the displayable part of the frame's luminance component.
    * - __u16
      - ``vertical_size``
      - The height of the displayable part of the frame's luminance component.
    * - __u32
      - ``vbv_buffer_size``
      - Used to calculate the required size of the video buffering verifier,
	defined (in bits) as: 16 * 1024 * vbv_buffer_size.
    * - __u16
      - ``profile_and_level_indication``
      - The current profile and level indication as extracted from the
	bitstream.
    * - __u8
      - ``progressive_sequence``
      - Indication that all the frames for the sequence are progressive instead
	of interlaced.
    * - __u8
      - ``chroma_format``
      - The chrominance sub-sampling format (1: 4:2:0, 2: 4:2:2, 3: 4:4:4).

.. c:type:: v4l2_mpeg2_picture

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_mpeg2_picture
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``picture_coding_type``
      - Picture coding type for the frame covered by the current slice
	(V4L2_MPEG2_PICTURE_CODING_TYPE_I, V4L2_MPEG2_PICTURE_CODING_TYPE_P or
	V4L2_MPEG2_PICTURE_CODING_TYPE_B).
    * - __u8
      - ``f_code[2][2]``
      - Motion vector codes.
    * - __u8
      - ``intra_dc_precision``
      - Precision of Discrete Cosine transform (0: 8 bits precision,
	1: 9 bits precision, 2: 10 bits precision, 3: 11 bits precision).
    * - __u8
      - ``picture_structure``
      - Picture structure (1: interlaced top field, 2: interlaced bottom field,
	3: progressive frame).
    * - __u8
      - ``top_field_first``
      - If set to 1 and interlaced stream, top field is output first.
    * - __u8
      - ``frame_pred_frame_dct``
      - If set to 1, only frame-DCT and frame prediction are used.
    * - __u8
      - ``concealment_motion_vectors``
      -  If set to 1, motion vectors are coded for intra macroblocks.
    * - __u8
      - ``q_scale_type``
      - This flag affects the inverse quantization process.
    * - __u8
      - ``intra_vlc_format``
      - This flag affects the decoding of transform coefficient data.
    * - __u8
      - ``alternate_scan``
      - This flag affects the decoding of transform coefficient data.
    * - __u8
      - ``repeat_first_field``
      - This flag affects the decoding process of progressive frames.
    * - __u16
      - ``progressive_frame``
      - Indicates whether the current frame is progressive.

``V4L2_CID_MPEG_VIDEO_MPEG2_QUANTIZATION (struct)``
    Specifies quantization matrices (as extracted from the bitstream) for the
    associated MPEG-2 slice data.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_mpeg2_quantization

.. cssclass:: longtable

.. tabularcolumns:: |p{1.2cm}|p{8.0cm}|p{7.4cm}|

.. raw:: latex

    \small

.. flat-table:: struct v4l2_ctrl_mpeg2_quantization
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``load_intra_quantiser_matrix``
      - One bit to indicate whether to load the ``intra_quantiser_matrix`` data.
    * - __u8
      - ``load_non_intra_quantiser_matrix``
      - One bit to indicate whether to load the ``non_intra_quantiser_matrix``
	data.
    * - __u8
      - ``load_chroma_intra_quantiser_matrix``
      - One bit to indicate whether to load the
	``chroma_intra_quantiser_matrix`` data, only relevant for non-4:2:0 YUV
	formats.
    * - __u8
      - ``load_chroma_non_intra_quantiser_matrix``
      - One bit to indicate whether to load the
	``chroma_non_intra_quantiser_matrix`` data, only relevant for non-4:2:0
	YUV formats.
    * - __u8
      - ``intra_quantiser_matrix[64]``
      - The quantization matrix coefficients for intra-coded frames, in zigzag
	scanning order. It is relevant for both luma and chroma components,
	although it can be superseded by the chroma-specific matrix for
	non-4:2:0 YUV formats.
    * - __u8
      - ``non_intra_quantiser_matrix[64]``
      - The quantization matrix coefficients for non-intra-coded frames, in
	zigzag scanning order. It is relevant for both luma and chroma
	components, although it can be superseded by the chroma-specific matrix
	for non-4:2:0 YUV formats.
    * - __u8
      - ``chroma_intra_quantiser_matrix[64]``
      - The quantization matrix coefficients for the chominance component of
	intra-coded frames, in zigzag scanning order. Only relevant for
	non-4:2:0 YUV formats.
    * - __u8
      - ``chroma_non_intra_quantiser_matrix[64]``
      - The quantization matrix coefficients for the chrominance component of
	non-intra-coded frames, in zigzag scanning order. Only relevant for
	non-4:2:0 YUV formats.

``V4L2_CID_FWHT_I_FRAME_QP (integer)``
    Quantization parameter for an I frame for FWHT. Valid range: from 1
    to 31.

``V4L2_CID_FWHT_P_FRAME_QP (integer)``
    Quantization parameter for a P frame for FWHT. Valid range: from 1
    to 31.

.. _v4l2-mpeg-vp8:

``V4L2_CID_MPEG_VIDEO_VP8_FRAME_HEADER (struct)``
    Specifies the frame parameters for the associated VP8 parsed frame data.
    This includes the necessary parameters for
    configuring a stateless hardware decoding pipeline for VP8.
    The bitstream parameters are defined according to :ref:`vp8`.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_vp8_frame_header

.. cssclass:: longtable

.. tabularcolumns:: |p{5.8cm}|p{4.8cm}|p{6.6cm}|

.. flat-table:: struct v4l2_ctrl_vp8_frame_header
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - struct :c:type:`v4l2_vp8_segment_header`
      - ``segment_header``
      - Structure with segment-based adjustments metadata.
    * - struct :c:type:`v4l2_vp8_loopfilter_header`
      - ``loopfilter_header``
      - Structure with loop filter level adjustments metadata.
    * - struct :c:type:`v4l2_vp8_quantization_header`
      - ``quant_header``
      - Structure with VP8 dequantization indices metadata.
    * - struct :c:type:`v4l2_vp8_entropy_header`
      - ``entropy_header``
      - Structure with VP8 entropy coder probabilities metadata.
    * - struct :c:type:`v4l2_vp8_entropy_coder_state`
      - ``coder_state``
      - Structure with VP8 entropy coder state.
    * - __u16
      - ``width``
      - The width of the frame. Must be set for all frames.
    * - __u16
      - ``height``
      - The height of the frame. Must be set for all frames.
    * - __u8
      - ``horizontal_scale``
      - Horizontal scaling factor.
    * - __u8
      - ``vertical_scaling factor``
      - Vertical scale.
    * - __u8
      - ``version``
      - Bitstream version.
    * - __u8
      - ``prob_skip_false``
      - Indicates the probability that the macroblock is not skipped.
    * - __u8
      - ``prob_intra``
      - Indicates the probability that a macroblock is intra-predicted.
    * - __u8
      - ``prob_last``
      - Indicates the probability that the last reference frame is used
        for inter-prediction
    * - __u8
      - ``prob_gf``
      - Indicates the probability that the golden reference frame is used
        for inter-prediction
    * - __u8
      - ``num_dct_parts``
      - Number of DCT coefficients partitions. Must be one of: 1, 2, 4, or 8.
    * - __u32
      - ``first_part_size``
      - Size of the first partition, i.e. the control partition.
    * - __u32
      - ``first_part_header_bits``
      - Size in bits of the first partition header portion.
    * - __u32
      - ``dct_part_sizes[8]``
      - DCT coefficients sizes.
    * - __u64
      - ``last_frame_ts``
      - Timestamp for the V4L2 capture buffer to use as last reference frame, used
        with inter-coded frames. The timestamp refers to the ``timestamp`` field in
	struct :c:type:`v4l2_buffer`. Use the :c:func:`v4l2_timeval_to_ns()`
	function to convert the struct :c:type:`timeval` in struct
	:c:type:`v4l2_buffer` to a __u64.
    * - __u64
      - ``golden_frame_ts``
      - Timestamp for the V4L2 capture buffer to use as last reference frame, used
        with inter-coded frames. The timestamp refers to the ``timestamp`` field in
	struct :c:type:`v4l2_buffer`. Use the :c:func:`v4l2_timeval_to_ns()`
	function to convert the struct :c:type:`timeval` in struct
	:c:type:`v4l2_buffer` to a __u64.
    * - __u64
      - ``alt_frame_ts``
      - Timestamp for the V4L2 capture buffer to use as alternate reference frame, used
        with inter-coded frames. The timestamp refers to the ``timestamp`` field in
	struct :c:type:`v4l2_buffer`. Use the :c:func:`v4l2_timeval_to_ns()`
	function to convert the struct :c:type:`timeval` in struct
	:c:type:`v4l2_buffer` to a __u64.
    * - __u64
      - ``flags``
      - See :ref:`Frame Header Flags <vp8_frame_header_flags>`

.. _vp8_frame_header_flags:

``Frame Header Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_VP8_FRAME_HEADER_FLAG_KEY_FRAME``
      - 0x01
      - Indicates if the frame is a key frame.
    * - ``V4L2_VP8_FRAME_HEADER_FLAG_EXPERIMENTAL``
      - 0x02
      - Experimental bitstream.
    * - ``V4L2_VP8_FRAME_HEADER_FLAG_SHOW_FRAME``
      - 0x04
      - Show frame flag, indicates if the frame is for display.
    * - ``V4L2_VP8_FRAME_HEADER_FLAG_MB_NO_SKIP_COEFF``
      - 0x08
      - Enable/disable skipping of macroblocks with no non-zero coefficients.
    * - ``V4L2_VP8_FRAME_HEADER_FLAG_SIGN_BIAS_GOLDEN``
      - 0x10
      - Sign of motion vectors when the golden frame is referenced.
    * - ``V4L2_VP8_FRAME_HEADER_FLAG_SIGN_BIAS_ALT``
      - 0x20
      - Sign of motion vectors when the alt frame is referenced.

.. c:type:: v4l2_vp8_entropy_coder_state

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_vp8_entropy_coder_state
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``range``
      -
    * - __u8
      - ``value``
      -
    * - __u8
      - ``bit_count``
      -
    * - __u8
      - ``padding``
      - Applications and drivers must set this to zero.

.. c:type:: v4l2_vp8_segment_header

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_vp8_segment_header
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __s8
      - ``quant_update[4]``
      - Signed quantizer value update.
    * - __s8
      - ``lf_update[4]``
      - Signed loop filter level value update.
    * - __u8
      - ``segment_probs[3]``
      - Segment probabilities.
    * - __u8
      - ``padding``
      - Applications and drivers must set this to zero.
    * - __u32
      - ``flags``
      - See :ref:`Segment Header Flags <vp8_segment_header_flags>`

.. _vp8_segment_header_flags:

``Segment Header Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_VP8_SEGMENT_HEADER_FLAG_ENABLED``
      - 0x01
      - Enable/disable segment-based adjustments.
    * - ``V4L2_VP8_SEGMENT_HEADER_FLAG_UPDATE_MAP``
      - 0x02
      - Indicates if the macroblock segmentation map is updated in this frame.
    * - ``V4L2_VP8_SEGMENT_HEADER_FLAG_UPDATE_FEATURE_DATA``
      - 0x04
      - Indicates if the segment feature data is updated in this frame.
    * - ``V4L2_VP8_SEGMENT_HEADER_FLAG_DELTA_VALUE_MODE``
      - 0x08
      - If is set, the segment feature data mode is delta-value.
        If cleared, it's absolute-value.

.. c:type:: v4l2_vp8_loopfilter_header

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_vp8_loopfilter_header
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __s8
      - ``ref_frm_delta[4]``
      - Reference adjustment (signed) delta value.
    * - __s8
      - ``mb_mode_delta[4]``
      - Macroblock prediction mode adjustment (signed) delta value.
    * - __u8
      - ``sharpness_level``
      - Sharpness level
    * - __u8
      - ``level``
      - Filter level
    * - __u16
      - ``padding``
      - Applications and drivers must set this to zero.
    * - __u32
      - ``flags``
      - See :ref:`Loopfilter Header Flags <vp8_loopfilter_header_flags>`

.. _vp8_loopfilter_header_flags:

``Loopfilter Header Flags``

.. cssclass:: longtable

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - ``V4L2_VP8_LF_HEADER_ADJ_ENABLE``
      - 0x01
      - Enable/disable macroblock-level loop filter adjustment.
    * - ``V4L2_VP8_LF_HEADER_DELTA_UPDATE``
      - 0x02
      - Indicates if the delta values used in an adjustment are updated.
    * - ``V4L2_VP8_LF_FILTER_TYPE_SIMPLE``
      - 0x04
      - If set, indicates the filter type is simple.
        If cleared, the filter type is normal.

.. c:type:: v4l2_vp8_quantization_header

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_vp8_quantization_header
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``y_ac_qi``
      - Luma AC coefficient table index.
    * - __s8
      - ``y_dc_delta``
      - Luma DC delta vaue.
    * - __s8
      - ``y2_dc_delta``
      - Y2 block DC delta value.
    * - __s8
      - ``y2_ac_delta``
      - Y2 block AC delta value.
    * - __s8
      - ``uv_dc_delta``
      - Chroma DC delta value.
    * - __s8
      - ``uv_ac_delta``
      - Chroma AC delta value.
    * - __u16
      - ``padding``
      - Applications and drivers must set this to zero.

.. c:type:: v4l2_vp8_entropy_header

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{6.3cm}|p{9.4cm}|

.. flat-table:: struct v4l2_vp8_entropy_header
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u8
      - ``coeff_probs[4][8][3][11]``
      - Coefficient update probabilities.
    * - __u8
      - ``y_mode_probs[4]``
      - Luma mode update probabilities.
    * - __u8
      - ``uv_mode_probs[3]``
      - Chroma mode update probabilities.
    * - __u8
      - ``mv_probs[2][19]``
      - MV decoding update probabilities.
    * - __u8
      - ``padding[3]``
      - Applications and drivers must set this to zero.

.. raw:: latex

    \normalsize


MFC 5.1 MPEG Controls
=====================

The following MPEG class controls deal with MPEG decoding and encoding
settings that are specific to the Multi Format Codec 5.1 device present
in the S5P family of SoCs by Samsung.


.. _mfc51-control-id:

MFC 5.1 Control IDs
-------------------

``V4L2_CID_MPEG_MFC51_VIDEO_DECODER_H264_DISPLAY_DELAY_ENABLE (boolean)``
    If the display delay is enabled then the decoder is forced to return
    a CAPTURE buffer (decoded frame) after processing a certain number
    of OUTPUT buffers. The delay can be set through
    ``V4L2_CID_MPEG_MFC51_VIDEO_DECODER_H264_DISPLAY_DELAY``. This
    feature can be used for example for generating thumbnails of videos.
    Applicable to the H264 decoder.

``V4L2_CID_MPEG_MFC51_VIDEO_DECODER_H264_DISPLAY_DELAY (integer)``
    Display delay value for H264 decoder. The decoder is forced to
    return a decoded frame after the set 'display delay' number of
    frames. If this number is low it may result in frames returned out
    of display order, in addition the hardware may still be using the
    returned buffer as a reference picture for subsequent frames.

``V4L2_CID_MPEG_MFC51_VIDEO_H264_NUM_REF_PIC_FOR_P (integer)``
    The number of reference pictures used for encoding a P picture.
    Applicable to the H264 encoder.

``V4L2_CID_MPEG_MFC51_VIDEO_PADDING (boolean)``
    Padding enable in the encoder - use a color instead of repeating
    border pixels. Applicable to encoders.

``V4L2_CID_MPEG_MFC51_VIDEO_PADDING_YUV (integer)``
    Padding color in the encoder. Applicable to encoders. The supplied
    32-bit integer is interpreted as follows (bit 0 = least significant
    bit):



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - Bit 0:7
      - V chrominance information
    * - Bit 8:15
      - U chrominance information
    * - Bit 16:23
      - Y luminance information
    * - Bit 24:31
      - Must be zero.



``V4L2_CID_MPEG_MFC51_VIDEO_RC_REACTION_COEFF (integer)``
    Reaction coefficient for MFC rate control. Applicable to encoders.

    .. note::

       #. Valid only when the frame level RC is enabled.

       #. For tight CBR, this field must be small (ex. 2 ~ 10). For
	  VBR, this field must be large (ex. 100 ~ 1000).

       #. It is not recommended to use the greater number than
	  FRAME_RATE * (10^9 / BIT_RATE).

``V4L2_CID_MPEG_MFC51_VIDEO_H264_ADAPTIVE_RC_DARK (boolean)``
    Adaptive rate control for dark region. Valid only when H.264 and
    macroblock level RC is enabled
    (``V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE``). Applicable to the H264
    encoder.

``V4L2_CID_MPEG_MFC51_VIDEO_H264_ADAPTIVE_RC_SMOOTH (boolean)``
    Adaptive rate control for smooth region. Valid only when H.264 and
    macroblock level RC is enabled
    (``V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE``). Applicable to the H264
    encoder.

``V4L2_CID_MPEG_MFC51_VIDEO_H264_ADAPTIVE_RC_STATIC (boolean)``
    Adaptive rate control for static region. Valid only when H.264 and
    macroblock level RC is enabled
    (``V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE``). Applicable to the H264
    encoder.

``V4L2_CID_MPEG_MFC51_VIDEO_H264_ADAPTIVE_RC_ACTIVITY (boolean)``
    Adaptive rate control for activity region. Valid only when H.264 and
    macroblock level RC is enabled
    (``V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE``). Applicable to the H264
    encoder.

.. _v4l2-mpeg-mfc51-video-frame-skip-mode:

``V4L2_CID_MPEG_MFC51_VIDEO_FRAME_SKIP_MODE``
    (enum)

enum v4l2_mpeg_mfc51_video_frame_skip_mode -
    Indicates in what conditions the encoder should skip frames. If
    encoding a frame would cause the encoded stream to be larger then a
    chosen data limit then the frame will be skipped. Possible values
    are:


.. tabularcolumns:: |p{9.2cm}|p{8.3cm}|

.. raw:: latex

    \small

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_MFC51_FRAME_SKIP_MODE_DISABLED``
      - Frame skip mode is disabled.
    * - ``V4L2_MPEG_MFC51_FRAME_SKIP_MODE_LEVEL_LIMIT``
      - Frame skip mode enabled and buffer limit is set by the chosen
	level and is defined by the standard.
    * - ``V4L2_MPEG_MFC51_FRAME_SKIP_MODE_BUF_LIMIT``
      - Frame skip mode enabled and buffer limit is set by the VBV
	(MPEG1/2/4) or CPB (H264) buffer size control.

.. raw:: latex

    \normalsize

``V4L2_CID_MPEG_MFC51_VIDEO_RC_FIXED_TARGET_BIT (integer)``
    Enable rate-control with fixed target bit. If this setting is
    enabled, then the rate control logic of the encoder will calculate
    the average bitrate for a GOP and keep it below or equal the set
    bitrate target. Otherwise the rate control logic calculates the
    overall average bitrate for the stream and keeps it below or equal
    to the set bitrate. In the first case the average bitrate for the
    whole stream will be smaller then the set bitrate. This is caused
    because the average is calculated for smaller number of frames, on
    the other hand enabling this setting will ensure that the stream
    will meet tight bandwidth constraints. Applicable to encoders.

.. _v4l2-mpeg-mfc51-video-force-frame-type:

``V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE``
    (enum)

enum v4l2_mpeg_mfc51_video_force_frame_type -
    Force a frame type for the next queued buffer. Applicable to
    encoders. Possible values are:

.. tabularcolumns:: |p{9.5cm}|p{8.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_MFC51_FORCE_FRAME_TYPE_DISABLED``
      - Forcing a specific frame type disabled.
    * - ``V4L2_MPEG_MFC51_FORCE_FRAME_TYPE_I_FRAME``
      - Force an I-frame.
    * - ``V4L2_MPEG_MFC51_FORCE_FRAME_TYPE_NOT_CODED``
      - Force a non-coded frame.


.. _v4l2-mpeg-fwht:

``V4L2_CID_MPEG_VIDEO_FWHT_PARAMS (struct)``
    Specifies the fwht parameters (as extracted from the bitstream) for the
    associated FWHT data. This includes the necessary parameters for
    configuring a stateless hardware decoding pipeline for FWHT.

    .. note::

       This compound control is not yet part of the public kernel API and
       it is expected to change.

.. c:type:: v4l2_ctrl_fwht_params

.. cssclass:: longtable

.. tabularcolumns:: |p{1.4cm}|p{4.3cm}|p{11.8cm}|

.. flat-table:: struct v4l2_ctrl_fwht_params
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u64
      - ``backward_ref_ts``
      - Timestamp of the V4L2 capture buffer to use as backward reference, used
        with P-coded frames. The timestamp refers to the
	``timestamp`` field in struct :c:type:`v4l2_buffer`. Use the
	:c:func:`v4l2_timeval_to_ns()` function to convert the struct
	:c:type:`timeval` in struct :c:type:`v4l2_buffer` to a __u64.
    * - __u32
      - ``version``
      - The version of the codec
    * - __u32
      - ``width``
      - The width of the frame
    * - __u32
      - ``height``
      - The height of the frame
    * - __u32
      - ``flags``
      - The flags of the frame, see :ref:`fwht-flags`.
    * - __u32
      - ``colorspace``
      - The colorspace of the frame, from enum :c:type:`v4l2_colorspace`.
    * - __u32
      - ``xfer_func``
      - The transfer function, from enum :c:type:`v4l2_xfer_func`.
    * - __u32
      - ``ycbcr_enc``
      - The Y'CbCr encoding, from enum :c:type:`v4l2_ycbcr_encoding`.
    * - __u32
      - ``quantization``
      - The quantization range, from enum :c:type:`v4l2_quantization`.



.. _fwht-flags:

FWHT Flags
============

.. cssclass:: longtable

.. tabularcolumns:: |p{6.8cm}|p{2.4cm}|p{8.3cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       3 1 4

    * - ``FWHT_FL_IS_INTERLACED``
      - 0x00000001
      - Set if this is an interlaced format
    * - ``FWHT_FL_IS_BOTTOM_FIRST``
      - 0x00000002
      - Set if this is a bottom-first (NTSC) interlaced format
    * - ``FWHT_FL_IS_ALTERNATE``
      - 0x00000004
      - Set if each 'frame' contains just one field
    * - ``FWHT_FL_IS_BOTTOM_FIELD``
      - 0x00000008
      - If FWHT_FL_IS_ALTERNATE was set, then this is set if this 'frame' is the
	bottom field, else it is the top field.
    * - ``FWHT_FL_LUMA_IS_UNCOMPRESSED``
      - 0x00000010
      - Set if the luma plane is uncompressed
    * - ``FWHT_FL_CB_IS_UNCOMPRESSED``
      - 0x00000020
      - Set if the cb plane is uncompressed
    * - ``FWHT_FL_CR_IS_UNCOMPRESSED``
      - 0x00000040
      - Set if the cr plane is uncompressed
    * - ``FWHT_FL_CHROMA_FULL_HEIGHT``
      - 0x00000080
      - Set if the chroma plane has the same height as the luma plane,
	else the chroma plane is half the height of the luma plane
    * - ``FWHT_FL_CHROMA_FULL_WIDTH``
      - 0x00000100
      - Set if the chroma plane has the same width as the luma plane,
	else the chroma plane is half the width of the luma plane
    * - ``FWHT_FL_ALPHA_IS_UNCOMPRESSED``
      - 0x00000200
      - Set if the alpha plane is uncompressed
    * - ``FWHT_FL_I_FRAME``
      - 0x00000400
      - Set if this is an I-frame
    * - ``FWHT_FL_COMPONENTS_NUM_MSK``
      - 0x00070000
      - A 4-values flag - the number of components - 1
    * - ``FWHT_FL_PIXENC_YUV``
      - 0x00080000
      - Set if the pixel encoding is YUV
    * - ``FWHT_FL_PIXENC_RGB``
      - 0x00100000
      - Set if the pixel encoding is RGB
    * - ``FWHT_FL_PIXENC_HSV``
      - 0x00180000
      - Set if the pixel encoding is HSV


CX2341x MPEG Controls
=====================

The following MPEG class controls deal with MPEG encoding settings that
are specific to the Conexant CX23415 and CX23416 MPEG encoding chips.


.. _cx2341x-control-id:

CX2341x Control IDs
-------------------

.. _v4l2-mpeg-cx2341x-video-spatial-filter-mode:

``V4L2_CID_MPEG_CX2341X_VIDEO_SPATIAL_FILTER_MODE``
    (enum)

enum v4l2_mpeg_cx2341x_video_spatial_filter_mode -
    Sets the Spatial Filter mode (default ``MANUAL``). Possible values
    are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_CX2341X_VIDEO_SPATIAL_FILTER_MODE_MANUAL``
      - Choose the filter manually
    * - ``V4L2_MPEG_CX2341X_VIDEO_SPATIAL_FILTER_MODE_AUTO``
      - Choose the filter automatically



``V4L2_CID_MPEG_CX2341X_VIDEO_SPATIAL_FILTER (integer (0-15))``
    The setting for the Spatial Filter. 0 = off, 15 = maximum. (Default
    is 0.)

.. _luma-spatial-filter-type:

``V4L2_CID_MPEG_CX2341X_VIDEO_LUMA_SPATIAL_FILTER_TYPE``
    (enum)

enum v4l2_mpeg_cx2341x_video_luma_spatial_filter_type -
    Select the algorithm to use for the Luma Spatial Filter (default
    ``1D_HOR``). Possible values:

.. tabularcolumns:: |p{14.5cm}|p{3.0cm}|

.. raw:: latex

    \small

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_CX2341X_VIDEO_LUMA_SPATIAL_FILTER_TYPE_OFF``
      - No filter
    * - ``V4L2_MPEG_CX2341X_VIDEO_LUMA_SPATIAL_FILTER_TYPE_1D_HOR``
      - One-dimensional horizontal
    * - ``V4L2_MPEG_CX2341X_VIDEO_LUMA_SPATIAL_FILTER_TYPE_1D_VERT``
      - One-dimensional vertical
    * - ``V4L2_MPEG_CX2341X_VIDEO_LUMA_SPATIAL_FILTER_TYPE_2D_HV_SEPARABLE``
      - Two-dimensional separable
    * - ``V4L2_MPEG_CX2341X_VIDEO_LUMA_SPATIAL_FILTER_TYPE_2D_SYM_NON_SEPARABLE``
      - Two-dimensional symmetrical non-separable

.. raw:: latex

    \normalsize



.. _chroma-spatial-filter-type:

``V4L2_CID_MPEG_CX2341X_VIDEO_CHROMA_SPATIAL_FILTER_TYPE``
    (enum)

enum v4l2_mpeg_cx2341x_video_chroma_spatial_filter_type -
    Select the algorithm for the Chroma Spatial Filter (default
    ``1D_HOR``). Possible values are:


.. tabularcolumns:: |p{14.0cm}|p{3.5cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_CX2341X_VIDEO_CHROMA_SPATIAL_FILTER_TYPE_OFF``
      - No filter
    * - ``V4L2_MPEG_CX2341X_VIDEO_CHROMA_SPATIAL_FILTER_TYPE_1D_HOR``
      - One-dimensional horizontal



.. _v4l2-mpeg-cx2341x-video-temporal-filter-mode:

``V4L2_CID_MPEG_CX2341X_VIDEO_TEMPORAL_FILTER_MODE``
    (enum)

enum v4l2_mpeg_cx2341x_video_temporal_filter_mode -
    Sets the Temporal Filter mode (default ``MANUAL``). Possible values
    are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_CX2341X_VIDEO_TEMPORAL_FILTER_MODE_MANUAL``
      - Choose the filter manually
    * - ``V4L2_MPEG_CX2341X_VIDEO_TEMPORAL_FILTER_MODE_AUTO``
      - Choose the filter automatically



``V4L2_CID_MPEG_CX2341X_VIDEO_TEMPORAL_FILTER (integer (0-31))``
    The setting for the Temporal Filter. 0 = off, 31 = maximum. (Default
    is 8 for full-scale capturing and 0 for scaled capturing.)

.. _v4l2-mpeg-cx2341x-video-median-filter-type:

``V4L2_CID_MPEG_CX2341X_VIDEO_MEDIAN_FILTER_TYPE``
    (enum)

enum v4l2_mpeg_cx2341x_video_median_filter_type -
    Median Filter Type (default ``OFF``). Possible values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_CX2341X_VIDEO_MEDIAN_FILTER_TYPE_OFF``
      - No filter
    * - ``V4L2_MPEG_CX2341X_VIDEO_MEDIAN_FILTER_TYPE_HOR``
      - Horizontal filter
    * - ``V4L2_MPEG_CX2341X_VIDEO_MEDIAN_FILTER_TYPE_VERT``
      - Vertical filter
    * - ``V4L2_MPEG_CX2341X_VIDEO_MEDIAN_FILTER_TYPE_HOR_VERT``
      - Horizontal and vertical filter
    * - ``V4L2_MPEG_CX2341X_VIDEO_MEDIAN_FILTER_TYPE_DIAG``
      - Diagonal filter



``V4L2_CID_MPEG_CX2341X_VIDEO_LUMA_MEDIAN_FILTER_BOTTOM (integer (0-255))``
    Threshold above which the luminance median filter is enabled
    (default 0)

``V4L2_CID_MPEG_CX2341X_VIDEO_LUMA_MEDIAN_FILTER_TOP (integer (0-255))``
    Threshold below which the luminance median filter is enabled
    (default 255)

``V4L2_CID_MPEG_CX2341X_VIDEO_CHROMA_MEDIAN_FILTER_BOTTOM (integer (0-255))``
    Threshold above which the chroma median filter is enabled (default
    0)

``V4L2_CID_MPEG_CX2341X_VIDEO_CHROMA_MEDIAN_FILTER_TOP (integer (0-255))``
    Threshold below which the chroma median filter is enabled (default
    255)

``V4L2_CID_MPEG_CX2341X_STREAM_INSERT_NAV_PACKETS (boolean)``
    The CX2341X MPEG encoder can insert one empty MPEG-2 PES packet into
    the stream between every four video frames. The packet size is 2048
    bytes, including the packet_start_code_prefix and stream_id
    fields. The stream_id is 0xBF (private stream 2). The payload
    consists of 0x00 bytes, to be filled in by the application. 0 = do
    not insert, 1 = insert packets.


VPX Control Reference
=====================

The VPX controls include controls for encoding parameters of VPx video
codec.


.. _vpx-control-id:

VPX Control IDs
---------------

.. _v4l2-vpx-num-partitions:

``V4L2_CID_MPEG_VIDEO_VPX_NUM_PARTITIONS``
    (enum)

enum v4l2_vp8_num_partitions -
    The number of token partitions to use in VP8 encoder. Possible
    values are:



.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_CID_MPEG_VIDEO_VPX_1_PARTITION``
      - 1 coefficient partition
    * - ``V4L2_CID_MPEG_VIDEO_VPX_2_PARTITIONS``
      - 2 coefficient partitions
    * - ``V4L2_CID_MPEG_VIDEO_VPX_4_PARTITIONS``
      - 4 coefficient partitions
    * - ``V4L2_CID_MPEG_VIDEO_VPX_8_PARTITIONS``
      - 8 coefficient partitions



``V4L2_CID_MPEG_VIDEO_VPX_IMD_DISABLE_4X4 (boolean)``
    Setting this prevents intra 4x4 mode in the intra mode decision.

.. _v4l2-vpx-num-ref-frames:

``V4L2_CID_MPEG_VIDEO_VPX_NUM_REF_FRAMES``
    (enum)

enum v4l2_vp8_num_ref_frames -
    The number of reference pictures for encoding P frames. Possible
    values are:

.. tabularcolumns:: |p{7.9cm}|p{9.6cm}|

.. raw:: latex

    \small

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_CID_MPEG_VIDEO_VPX_1_REF_FRAME``
      - Last encoded frame will be searched
    * - ``V4L2_CID_MPEG_VIDEO_VPX_2_REF_FRAME``
      - Two frames will be searched among the last encoded frame, the
	golden frame and the alternate reference (altref) frame. The
	encoder implementation will decide which two are chosen.
    * - ``V4L2_CID_MPEG_VIDEO_VPX_3_REF_FRAME``
      - The last encoded frame, the golden frame and the altref frame will
	be searched.

.. raw:: latex

    \normalsize



``V4L2_CID_MPEG_VIDEO_VPX_FILTER_LEVEL (integer)``
    Indicates the loop filter level. The adjustment of the loop filter
    level is done via a delta value against a baseline loop filter
    value.

``V4L2_CID_MPEG_VIDEO_VPX_FILTER_SHARPNESS (integer)``
    This parameter affects the loop filter. Anything above zero weakens
    the deblocking effect on the loop filter.

``V4L2_CID_MPEG_VIDEO_VPX_GOLDEN_FRAME_REF_PERIOD (integer)``
    Sets the refresh period for the golden frame. The period is defined
    in number of frames. For a value of 'n', every nth frame starting
    from the first key frame will be taken as a golden frame. For eg.
    for encoding sequence of 0, 1, 2, 3, 4, 5, 6, 7 where the golden
    frame refresh period is set as 4, the frames 0, 4, 8 etc will be
    taken as the golden frames as frame 0 is always a key frame.

.. _v4l2-vpx-golden-frame-sel:

``V4L2_CID_MPEG_VIDEO_VPX_GOLDEN_FRAME_SEL``
    (enum)

enum v4l2_vp8_golden_frame_sel -
    Selects the golden frame for encoding. Possible values are:

.. raw:: latex

    \scriptsize

.. tabularcolumns:: |p{9.0cm}|p{8.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_CID_MPEG_VIDEO_VPX_GOLDEN_FRAME_USE_PREV``
      - Use the (n-2)th frame as a golden frame, current frame index being
	'n'.
    * - ``V4L2_CID_MPEG_VIDEO_VPX_GOLDEN_FRAME_USE_REF_PERIOD``
      - Use the previous specific frame indicated by
	``V4L2_CID_MPEG_VIDEO_VPX_GOLDEN_FRAME_REF_PERIOD`` as a
	golden frame.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_VPX_MIN_QP (integer)``
    Minimum quantization parameter for VP8.

``V4L2_CID_MPEG_VIDEO_VPX_MAX_QP (integer)``
    Maximum quantization parameter for VP8.

``V4L2_CID_MPEG_VIDEO_VPX_I_FRAME_QP (integer)``
    Quantization parameter for an I frame for VP8.

``V4L2_CID_MPEG_VIDEO_VPX_P_FRAME_QP (integer)``
    Quantization parameter for a P frame for VP8.

.. _v4l2-mpeg-video-vp8-profile:

``V4L2_CID_MPEG_VIDEO_VP8_PROFILE``
    (enum)

enum v4l2_mpeg_video_vp8_profile -
    This control allows selecting the profile for VP8 encoder.
    This is also used to enumerate supported profiles by VP8 encoder or decoder.
    Possible values are:

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_VP8_PROFILE_0``
      - Profile 0
    * - ``V4L2_MPEG_VIDEO_VP8_PROFILE_1``
      - Profile 1
    * - ``V4L2_MPEG_VIDEO_VP8_PROFILE_2``
      - Profile 2
    * - ``V4L2_MPEG_VIDEO_VP8_PROFILE_3``
      - Profile 3

.. _v4l2-mpeg-video-vp9-profile:

``V4L2_CID_MPEG_VIDEO_VP9_PROFILE``
    (enum)

enum v4l2_mpeg_video_vp9_profile -
    This control allows selecting the profile for VP9 encoder.
    This is also used to enumerate supported profiles by VP9 encoder or decoder.
    Possible values are:

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_VP9_PROFILE_0``
      - Profile 0
    * - ``V4L2_MPEG_VIDEO_VP9_PROFILE_1``
      - Profile 1
    * - ``V4L2_MPEG_VIDEO_VP9_PROFILE_2``
      - Profile 2
    * - ``V4L2_MPEG_VIDEO_VP9_PROFILE_3``
      - Profile 3


High Efficiency Video Coding (HEVC/H.265) Control Reference
===========================================================

The HEVC/H.265 controls include controls for encoding parameters of HEVC/H.265
video codec.


.. _hevc-control-id:

HEVC/H.265 Control IDs
----------------------

``V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP (integer)``
    Minimum quantization parameter for HEVC.
    Valid range: from 0 to 51.

``V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP (integer)``
    Maximum quantization parameter for HEVC.
    Valid range: from 0 to 51.

``V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP (integer)``
    Quantization parameter for an I frame for HEVC.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP (integer)``
    Quantization parameter for a P frame for HEVC.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP (integer)``
    Quantization parameter for a B frame for HEVC.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_QP (boolean)``
    HIERARCHICAL_QP allows the host to specify the quantization parameter
    values for each temporal layer through HIERARCHICAL_QP_LAYER. This is
    valid only if HIERARCHICAL_CODING_LAYER is greater than 1. Setting the
    control value to 1 enables setting of the QP values for the layers.

.. _v4l2-hevc-hier-coding-type:

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_TYPE``
    (enum)

enum v4l2_mpeg_video_hevc_hier_coding_type -
    Selects the hierarchical coding type for encoding. Possible values are:

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{9.0cm}|p{8.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_HIERARCHICAL_CODING_B``
      - Use the B frame for hierarchical coding.
    * - ``V4L2_MPEG_VIDEO_HEVC_HIERARCHICAL_CODING_P``
      - Use the P frame for hierarchical coding.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_LAYER (integer)``
    Selects the hierarchical coding layer. In normal encoding
    (non-hierarchial coding), it should be zero. Possible values are [0, 6].
    0 indicates HIERARCHICAL CODING LAYER 0, 1 indicates HIERARCHICAL CODING
    LAYER 1 and so on.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L0_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 0.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L1_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 1.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L2_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 2.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L3_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 3.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L4_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 4.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L5_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 5.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L6_QP (integer)``
    Indicates quantization parameter for hierarchical coding layer 6.
    Valid range: [V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP].

.. _v4l2-hevc-profile:

``V4L2_CID_MPEG_VIDEO_HEVC_PROFILE``
    (enum)

enum v4l2_mpeg_video_hevc_profile -
    Select the desired profile for HEVC encoder.

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{9.0cm}|p{8.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN``
      - Main profile.
    * - ``V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE``
      - Main still picture profile.
    * - ``V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10``
      - Main 10 profile.

.. raw:: latex

    \normalsize


.. _v4l2-hevc-level:

``V4L2_CID_MPEG_VIDEO_HEVC_LEVEL``
    (enum)

enum v4l2_mpeg_video_hevc_level -
    Selects the desired level for HEVC encoder.

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{9.0cm}|p{8.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_1``
      - Level 1.0
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_2``
      - Level 2.0
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1``
      - Level 2.1
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_3``
      - Level 3.0
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1``
      - Level 3.1
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_4``
      - Level 4.0
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1``
      - Level 4.1
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_5``
      - Level 5.0
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1``
      - Level 5.1
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2``
      - Level 5.2
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_6``
      - Level 6.0
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1``
      - Level 6.1
    * - ``V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2``
      - Level 6.2

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_HEVC_FRAME_RATE_RESOLUTION (integer)``
    Indicates the number of evenly spaced subintervals, called ticks, within
    one second. This is a 16 bit unsigned integer and has a maximum value up to
    0xffff and a minimum value of 1.

.. _v4l2-hevc-tier:

``V4L2_CID_MPEG_VIDEO_HEVC_TIER``
    (enum)

enum v4l2_mpeg_video_hevc_tier -
    TIER_FLAG specifies tiers information of the HEVC encoded picture. Tier
    were made to deal with applications that differ in terms of maximum bit
    rate. Setting the flag to 0 selects HEVC tier as Main tier and setting
    this flag to 1 indicates High tier. High tier is for applications requiring
    high bit rates.

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{9.0cm}|p{8.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_TIER_MAIN``
      - Main tier.
    * - ``V4L2_MPEG_VIDEO_HEVC_TIER_HIGH``
      - High tier.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_HEVC_MAX_PARTITION_DEPTH (integer)``
    Selects HEVC maximum coding unit depth.

.. _v4l2-hevc-loop-filter-mode:

``V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE``
    (enum)

enum v4l2_mpeg_video_hevc_loop_filter_mode -
    Loop filter mode for HEVC encoder. Possible values are:

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{12.1cm}|p{5.4cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED``
      - Loop filter is disabled.
    * - ``V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED``
      - Loop filter is enabled.
    * - ``V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY``
      - Loop filter is disabled at the slice boundary.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2 (integer)``
    Selects HEVC loop filter beta offset. The valid range is [-6, +6].

``V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2 (integer)``
    Selects HEVC loop filter tc offset. The valid range is [-6, +6].

.. _v4l2-hevc-refresh-type:

``V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_TYPE``
    (enum)

enum v4l2_mpeg_video_hevc_hier_refresh_type -
    Selects refresh type for HEVC encoder.
    Host has to specify the period into
    V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD.

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{8.0cm}|p{9.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_REFRESH_NONE``
      - Use the B frame for hierarchical coding.
    * - ``V4L2_MPEG_VIDEO_HEVC_REFRESH_CRA``
      - Use CRA (Clean Random Access Unit) picture encoding.
    * - ``V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR``
      - Use IDR (Instantaneous Decoding Refresh) picture encoding.

.. raw:: latex

    \normalsize


``V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD (integer)``
    Selects the refresh period for HEVC encoder.
    This specifies the number of I pictures between two CRA/IDR pictures.
    This is valid only if REFRESH_TYPE is not 0.

``V4L2_CID_MPEG_VIDEO_HEVC_LOSSLESS_CU (boolean)``
    Indicates HEVC lossless encoding. Setting it to 0 disables lossless
    encoding. Setting it to 1 enables lossless encoding.

``V4L2_CID_MPEG_VIDEO_HEVC_CONST_INTRA_PRED (boolean)``
    Indicates constant intra prediction for HEVC encoder. Specifies the
    constrained intra prediction in which intra largest coding unit (LCU)
    prediction is performed by using residual data and decoded samples of
    neighboring intra LCU only. Setting the value to 1 enables constant intra
    prediction and setting the value to 0 disables constant intra prediction.

``V4L2_CID_MPEG_VIDEO_HEVC_WAVEFRONT (boolean)``
    Indicates wavefront parallel processing for HEVC encoder. Setting it to 0
    disables the feature and setting it to 1 enables the wavefront parallel
    processing.

``V4L2_CID_MPEG_VIDEO_HEVC_GENERAL_PB (boolean)``
    Setting the value to 1 enables combination of P and B frame for HEVC
    encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_TEMPORAL_ID (boolean)``
    Indicates temporal identifier for HEVC encoder which is enabled by
    setting the value to 1.

``V4L2_CID_MPEG_VIDEO_HEVC_STRONG_SMOOTHING (boolean)``
    Indicates bi-linear interpolation is conditionally used in the intra
    prediction filtering process in the CVS when set to 1. Indicates bi-linear
    interpolation is not used in the CVS when set to 0.

``V4L2_CID_MPEG_VIDEO_HEVC_MAX_NUM_MERGE_MV_MINUS1 (integer)``
    Indicates maximum number of merge candidate motion vectors.
    Values are from 0 to 4.

``V4L2_CID_MPEG_VIDEO_HEVC_TMV_PREDICTION (boolean)``
    Indicates temporal motion vector prediction for HEVC encoder. Setting it to
    1 enables the prediction. Setting it to 0 disables the prediction.

``V4L2_CID_MPEG_VIDEO_HEVC_WITHOUT_STARTCODE (boolean)``
    Specifies if HEVC generates a stream with a size of the length field
    instead of start code pattern. The size of the length field is configurable
    through the V4L2_CID_MPEG_VIDEO_HEVC_SIZE_OF_LENGTH_FIELD control. Setting
    the value to 0 disables encoding without startcode pattern. Setting the
    value to 1 will enables encoding without startcode pattern.

.. _v4l2-hevc-size-of-length-field:

``V4L2_CID_MPEG_VIDEO_HEVC_SIZE_OF_LENGTH_FIELD``
(enum)

enum v4l2_mpeg_video_hevc_size_of_length_field -
    Indicates the size of length field.
    This is valid when encoding WITHOUT_STARTCODE_ENABLE is enabled.

.. raw:: latex

    \footnotesize

.. tabularcolumns:: |p{6.0cm}|p{11.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - ``V4L2_MPEG_VIDEO_HEVC_SIZE_0``
      - Generate start code pattern (Normal).
    * - ``V4L2_MPEG_VIDEO_HEVC_SIZE_1``
      - Generate size of length field instead of start code pattern and length is 1.
    * - ``V4L2_MPEG_VIDEO_HEVC_SIZE_2``
      - Generate size of length field instead of start code pattern and length is 2.
    * - ``V4L2_MPEG_VIDEO_HEVC_SIZE_4``
      - Generate size of length field instead of start code pattern and length is 4.

.. raw:: latex

    \normalsize

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L0_BR (integer)``
    Indicates bit rate for hierarchical coding layer 0 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L1_BR (integer)``
    Indicates bit rate for hierarchical coding layer 1 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L2_BR (integer)``
    Indicates bit rate for hierarchical coding layer 2 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L3_BR (integer)``
    Indicates bit rate for hierarchical coding layer 3 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L4_BR (integer)``
    Indicates bit rate for hierarchical coding layer 4 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L5_BR (integer)``
    Indicates bit rate for hierarchical coding layer 5 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L6_BR (integer)``
    Indicates bit rate for hierarchical coding layer 6 for HEVC encoder.

``V4L2_CID_MPEG_VIDEO_REF_NUMBER_FOR_PFRAMES (integer)``
    Selects number of P reference pictures required for HEVC encoder.
    P-Frame can use 1 or 2 frames for reference.

``V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR (integer)``
    Indicates whether to generate SPS and PPS at every IDR. Setting it to 0
    disables generating SPS and PPS at every IDR. Setting it to one enables
    generating SPS and PPS at every IDR.
