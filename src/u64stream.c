/*****************************************************************************
 * u64stream.c : Ultimate 64 / Commodore 64 Ultimate raw video+audio demux
 *****************************************************************************
 * Copyright (C) 2026 vlc-u64stream contributors
 *
 * Author: Jusii <jussi@alanara.fi>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Note: this plugin is dlopen()ed by VLC at runtime. libvlccore is
 * LGPL-2.1-or-later, which permits linking under any compatible terms,
 * including Apache-2.0. We do not statically link any GPL-only code.
 *
 * Protocol reference (factual / non-copyrightable):
 *   https://1541u-documentation.readthedocs.io/en/latest/data_streams.html
 *
 * Video datagram (UDP, 780 bytes total, default port 11000):
 *   off  size  field
 *    0    2   sequence number       (u16 LE)
 *    2    2   frame number          (u16 LE)
 *    4    2   line number           (u16 LE; bit15 = last packet of frame)
 *    6    2   pixels per line       (u16 LE; constant 384)
 *    8    1   lines per packet      (u8;     constant 4)
 *    9    1   bits per pixel        (u8;     constant 4)
 *   10    2   encoding              (u16;    constant 0)
 *   12  768   pixel payload (4 lines x 384 px x 4 bits, nibble-packed:
 *             the LOW nibble of each byte is the FIRST/leftmost pixel)
 *
 * A complete PAL frame is 384x272 = 68 packets; NTSC is 384x240 = 60 packets.
 *
 * Audio datagram (UDP, 770 bytes total, default port 11001):
 *   off  size  field
 *    0    2   sequence number       (u16 LE)
 *    2  768   192 stereo S16LE samples (L,R interleaved); 2ch * 16-bit * 192
 *
 * Sample rate: ~47983 Hz (PAL) / ~47940 Hz (NTSC), close to 48 kHz.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  /* strcasecmp() pulled in by <vlc_stream.h> */

#include <strings.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_network.h>
#include <vlc_block.h>
#include <vlc_url.h>
#include <vlc_aout.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdatomic.h>

/* VLC's plugin macros expect these gettext shims to exist in the TU.
 * We don't ship translations, so map them to identity / vlc_gettext. */
#ifndef N_
# define N_(s) (s)
#endif
#ifndef _
# define _(s) vlc_gettext(s)
#endif

/* Wall-clock helper. mdate() is the VLC 3.x name; 4.x renamed it. */
#if !defined(LIBVLC_VERSION_MAJOR) || LIBVLC_VERSION_MAJOR < 4
# define u64s_tick_now()  mdate()
#else
# define u64s_tick_now()  vlc_tick_now()
#endif

/* ---- Constants ---------------------------------------------------------- */

#define U64S_PKT_SIZE_VIDEO   780
#define U64S_HDR_SIZE_VIDEO    12
#define U64S_PAYLOAD_VIDEO    768
#define U64S_LINES_PER_PKT     4
#define U64S_PIXELS_PER_LN   384

#define U64S_PKT_SIZE_AUDIO   770
#define U64S_HDR_SIZE_AUDIO     2
#define U64S_PAYLOAD_AUDIO    768   /* 192 stereo s16 samples */
#define U64S_AUDIO_FRAMES_PKT 192

#define U64S_DEFAULT_PORT_VIDEO 11000
/* Default audio port = video port + 1, unless overridden */

#define U64S_PAL_HEIGHT       272
#define U64S_NTSC_HEIGHT      240
#define U64S_FRAMEBUF_HEIGHT  272   /* always allocate the larger of the two */

#define U64S_PAL_FPS           50
#define U64S_NTSC_FPS          60
#define U64S_PAL_AUDIO_RATE  47983
#define U64S_NTSC_AUDIO_RATE 47940

/* Mode option values. */
#define U64S_MODE_AUTO  -1
#define U64S_MODE_PAL    0
#define U64S_MODE_NTSC   1

/* Default sample-aspect-ratio per mode.
 *
 * We default to "full 384x{272|240} buffer displays at 4:3" rather than
 * the strictly-pixel-accurate VIC-II ratio, because that's what real
 * C64 monitors looked like and what most users expect. Math:
 *   SAR = (4/3) / (W/H) = 4*H / (3*W)
 *   PAL:  4*272 / (3*384) = 1088 / 1152 = 17:18  ~= 0.9444
 *   NTSC: 4*240 / (3*384) =  960 / 1152 =  5:6   ~= 0.8333
 *
 * For purists who want the strict VIC-II pixel aspect (PAL ~0.9365,
 * NTSC ~0.75 as VICE uses), pass --u64stream-sar-num=117
 * --u64stream-sar-den=125 (or 3:4 for NTSC). */
#define U64S_PAL_SAR_NUM    17
#define U64S_PAL_SAR_DEN    18
#define U64S_NTSC_SAR_NUM    5
#define U64S_NTSC_SAR_DEN    6

/* Process-global instance counter. VLC opens any URL multiple times for
 * the same playlist entry (preparse / art-fetch / playback). Each open
 * binds the same UDP port with SO_REUSEADDR, splitting incoming datagrams
 * between sockets and corrupting playback. We allow only one live instance
 * per process; later opens get rejected and VLC routes traffic to the
 * surviving one. */
static atomic_int u64s_live_instances = 0;

/* C64 ("Pepto") palette, 16 colors, 0xRRGGBB. */
static const uint32_t u64s_palette[16] = {
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
    0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444,
    0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};

/* ---- Module options ----------------------------------------------------- */

#define U64S_CFG_PREFIX "u64stream-"

#define U64S_PORT_TEXT       N_("UDP video port")
#define U64S_PORT_LONG       N_("Local UDP port to receive Ultimate-64 video " \
                                "datagrams on. Overrides the URL's port.")
#define U64S_AUDIO_PORT_TEXT N_("UDP audio port (0 = video port + 1)")
#define U64S_AUDIO_PORT_LONG N_("Local UDP port for the audio stream. " \
                                "Set to 0 to derive from the video port (+1).")
#define U64S_AUDIO_GROUP_TEXT N_("Audio multicast group (empty = video " \
                                 "group with last IPv4 octet +1)")
#define U64S_AUDIO_GROUP_LONG N_("Multicast group for the audio stream. " \
                                 "When the video URL uses a multicast group " \
                                 "and this is empty, the audio group is " \
                                 "derived by incrementing the last IPv4 " \
                                 "octet (matching the Ultimate 64's default " \
                                 "of e.g. 239.0.1.64 video -> 239.0.1.65 " \
                                 "audio). Set explicitly to override.")
#define U64S_NO_AUDIO_TEXT   N_("Disable audio")
#define U64S_NO_AUDIO_LONG   N_("Don't open the audio UDP socket.")
#define U64S_NO_VIDEO_TEXT   N_("Disable video")
#define U64S_NO_VIDEO_LONG   N_("Don't open the video UDP socket. " \
                                "Useful for audio-only listening.")
#define U64S_ON_LOSS_TEXT    N_("On packet loss: 0=persist last frame, " \
                                "1=clear missing rows to black")
#define U64S_ON_LOSS_LONG    N_("If a video packet is dropped, by default " \
                                "the affected rows show what was there in " \
                                "the previous frame (smoother). Set to 1 " \
                                "to instead clear the framebuffer to black " \
                                "at every frame boundary so missing rows " \
                                "are obvious.")
#define U64S_CTLHOST_TEXT    N_("C64U control host (telnet) for auto-start")
#define U64S_CTLHOST_LONG    N_("If set, after opening the UDP sockets we " \
                                "TCP-connect to this host (default port 23) " \
                                "and send the keystroke sequence the U64's " \
                                "F5 menu uses to start the video+audio " \
                                "stream. Format: 'host' or 'host:port'. " \
                                "FRAGILE: depends on the U64 firmware menu " \
                                "layout. Leave empty if you start the " \
                                "stream manually from the device.")
#define U64S_MODE_TEXT       N_("Video mode (-1=auto, 0=PAL, 1=NTSC)")
#define U64S_MODE_LONG       N_("Pick PAL (50Hz/272 lines) or NTSC " \
                                "(60Hz/240 lines), or -1 to detect from the " \
                                "first complete frame.")
#define U64S_SOURCE_TEXT     N_("Source IP filter (empty = any)")
#define U64S_SOURCE_LONG     N_("Only accept packets whose source IP matches " \
                                "this address. Useful when several U64 " \
                                "devices broadcast on the same LAN.")
#define U64S_SARNUM_TEXT     N_("Sample aspect ratio numerator (0 = default)")
#define U64S_SARNUM_LONG     N_("Override the pixel aspect ratio numerator. " \
                                "Defaults make the full 384x272 (PAL) or " \
                                "384x240 (NTSC) frame display at 4:3 — " \
                                "PAL 17:18 (~0.944), NTSC 5:6 (~0.833). " \
                                "For strict VIC-II pixel-accurate aspect " \
                                "use 117:125 (PAL) or 3:4 (NTSC). " \
                                "1:1 gives raw square pixels.")
#define U64S_SARDEN_TEXT     N_("Sample aspect ratio denominator (0 = default)")
#define U64S_SARDEN_LONG     N_("See --u64stream-sar-num.")

/* ---- Module entry points ------------------------------------------------ */

static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin()
    set_shortname( "U64Stream" )
    set_description( N_("Ultimate 64 raw video+audio stream") )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_ACCESS )
    set_capability( "access_demux", 0 )
    add_shortcut( "u64", "u64stream" )

    add_integer( U64S_CFG_PREFIX "port", U64S_DEFAULT_PORT_VIDEO,
                 U64S_PORT_TEXT, U64S_PORT_LONG, false )
    add_integer( U64S_CFG_PREFIX "audio-port", 0,
                 U64S_AUDIO_PORT_TEXT, U64S_AUDIO_PORT_LONG, false )
    add_string ( U64S_CFG_PREFIX "audio-group", "",
                 U64S_AUDIO_GROUP_TEXT, U64S_AUDIO_GROUP_LONG, false )
    add_bool   ( U64S_CFG_PREFIX "no-audio", false,
                 U64S_NO_AUDIO_TEXT, U64S_NO_AUDIO_LONG, false )
    add_bool   ( U64S_CFG_PREFIX "no-video", false,
                 U64S_NO_VIDEO_TEXT, U64S_NO_VIDEO_LONG, false )
    add_integer( U64S_CFG_PREFIX "mode", U64S_MODE_AUTO,
                 U64S_MODE_TEXT, U64S_MODE_LONG, false )
    add_string ( U64S_CFG_PREFIX "source", "",
                 U64S_SOURCE_TEXT, U64S_SOURCE_LONG, false )
    add_integer( U64S_CFG_PREFIX "on-loss", 0,
                 U64S_ON_LOSS_TEXT, U64S_ON_LOSS_LONG, false )
    add_string ( U64S_CFG_PREFIX "control-host", "",
                 U64S_CTLHOST_TEXT, U64S_CTLHOST_LONG, false )
    add_integer( U64S_CFG_PREFIX "sar-num", 0,
                 U64S_SARNUM_TEXT, U64S_SARNUM_LONG, true )
    add_integer( U64S_CFG_PREFIX "sar-den", 0,
                 U64S_SARDEN_TEXT, U64S_SARDEN_LONG, true )

    set_callbacks( Open, Close )
vlc_module_end()

/* ---- Per-instance state ------------------------------------------------- */
/* `demux_sys_t` is forward-declared by <vlc_common.h>; we must define the
 * tagged struct rather than typedef an anonymous one. */

struct demux_sys_t
{
    /* Sockets. fd_audio == -1 if audio is disabled. */
    int             fd_video;
    int             fd_audio;

    /* Optional IPv4 source filter. 0 = accept any. */
    bool            filter_src;
    struct in_addr  src;

    /* Detected/forced video mode. */
    int             mode;           /* U64S_MODE_AUTO/PAL/NTSC */
    bool            detected;       /* true once mode is fixed */
    int             width;          /* always 384 */
    int             height;         /* 272 PAL / 240 NTSC */
    int             fps;            /* 50 / 60 */
    int             audio_rate;     /* 47983 / 47940 */

    /* Aspect overrides (0 = use mode default). */
    int             sar_num_override;
    int             sar_den_override;

    /* Video state. */
    es_out_id_t    *es_video;
    uint8_t        *frame_rgba;     /* 384 * U64S_FRAMEBUF_HEIGHT * 4 bytes */
    uint64_t        v_frames;
    uint16_t        v_last_seq;
    bool            v_saw_seq;
    bool            v_first_boundary_seen; /* first bit15 marks "we joined
                                              the stream"; emit only frames
                                              captured cleanly between two
                                              boundaries. */
    bool            on_loss_clear;  /* true: memset framebuffer to 0 on
                                       every new frame start. */

    /* Audio state. */
    es_out_id_t    *es_audio;
    uint64_t        a_samples;      /* total stereo frames emitted */
    uint16_t        a_last_seq;
    bool            a_saw_seq;

    /* ES bookkeeping. */
    bool            es_announced;

    /* Last PCR we sent, to keep it monotonic across both streams. */
    int64_t         last_pcr;

    /* Per-ES wall-clock anchors for PTS computation. We sample mdate() at
     * the first emitted block of each ES so subsequent PTSes track real
     * elapsed time. Each ES needs its own anchor: audio typically starts
     * emitting later than video (we wait for mode detection), and using a
     * shared anchor would put audio's first PTS in the past relative to
     * wall-clock — VLC then drops the entire audio stream. */
    int64_t         v_t0_us;
    int64_t         a_t0_us;
};

/* ---- UDP socket helpers ------------------------------------------------- */

/* Open a UDP datagram socket for receiving.
 *
 *   bindhost: local address to bind, or "" for any.
 *   group:    multicast group to join, or NULL for plain unicast.
 *   port:     UDP port (used for both bind and group).
 */
static int u64s_open_socket( demux_t *demux, const char *bindhost,
                             const char *group, int port,
                             const char *label )
{
    if( port <= 0 || port > 65535 )
    {
        msg_Err( demux, "invalid %s port %d", label, port );
        return -1;
    }
    /* net_OpenDgram(obj, bind_host, bind_port, server, server_port, proto)
     * For multicast: server = group address; libvlccore handles
     * IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP for us. */
    int fd = net_OpenDgram( demux,
                            bindhost ? bindhost : "", port,
                            group ? group : "",      port,
                            IPPROTO_UDP );
    if( fd < 0 )
    {
        msg_Err( demux, "cannot bind UDP %s:%d (group %s) for %s (%s)",
                 bindhost && *bindhost ? bindhost : "*", port,
                 group   && *group    ? group    : "none", label,
                 vlc_strerror_c(errno) );
        return -1;
    }
    if( group && *group )
        msg_Info( demux,
                  "U64 %s: joined multicast %s:%d on bind %s",
                  label, group, port,
                  bindhost && *bindhost ? bindhost : "*" );
    else
        msg_Info( demux, "U64 %s: listening on UDP %s:%d", label,
                  bindhost && *bindhost ? bindhost : "*", port );
    return fd;
}

/* recvfrom() one datagram with MSG_DONTWAIT (non-blocking). Returns:
 *   >0  bytes read
 *    0  packet dropped by source filter
 *   -1  with errno=EAGAIN/EWOULDBLOCK  → queue is empty (call drained)
 *   -1  with another errno              → real error
 * The drain loop in Demux() relies on the EAGAIN signal. */
static ssize_t u64s_recv_filtered( demux_t *demux, int fd,
                                   void *buf, size_t buflen )
{
    demux_sys_t *sys = demux->p_sys;
    struct sockaddr_in sa;
    socklen_t sl = (socklen_t)sizeof(sa);
    ssize_t n = recvfrom( fd, buf, buflen, MSG_DONTWAIT,
                          (struct sockaddr *)&sa, &sl );
    if( n < 0 )
        return n;
    if( sys->filter_src && sa.sin_family == AF_INET
                        && sa.sin_addr.s_addr != sys->src.s_addr )
    {
        return 0; /* drop */
    }
    return n;
}

/* ---- Mode detection / ES announcement ---------------------------------- */

static void u64s_apply_mode( demux_sys_t *sys, int mode )
{
    sys->mode = mode;
    if( mode == U64S_MODE_NTSC )
    {
        sys->height     = U64S_NTSC_HEIGHT;
        sys->fps        = U64S_NTSC_FPS;
        sys->audio_rate = U64S_NTSC_AUDIO_RATE;
    }
    else /* default to PAL */
    {
        sys->height     = U64S_PAL_HEIGHT;
        sys->fps        = U64S_PAL_FPS;
        sys->audio_rate = U64S_PAL_AUDIO_RATE;
    }
    sys->detected = true;
}

static void u64s_announce_es( demux_t *demux )
{
    demux_sys_t *sys = demux->p_sys;
    if( sys->es_announced )
        return;

    /* Pick SAR. Override > built-in per-mode default. */
    int sar_num = sys->sar_num_override;
    int sar_den = sys->sar_den_override;
    if( sar_num <= 0 || sar_den <= 0 )
    {
        if( sys->mode == U64S_MODE_NTSC )
        {
            sar_num = U64S_NTSC_SAR_NUM;
            sar_den = U64S_NTSC_SAR_DEN;
        }
        else
        {
            sar_num = U64S_PAL_SAR_NUM;
            sar_den = U64S_PAL_SAR_DEN;
        }
    }

    /* Video ES */
    es_format_t v;
    es_format_Init( &v, VIDEO_ES, VLC_CODEC_RGBA );
    v.video.i_chroma          = VLC_CODEC_RGBA;
    v.video.i_width           = sys->width;
    v.video.i_height          = sys->height;
    v.video.i_visible_width   = sys->width;
    v.video.i_visible_height  = sys->height;
    v.video.i_sar_num         = sar_num;
    v.video.i_sar_den         = sar_den;
    v.video.i_frame_rate      = sys->fps;
    v.video.i_frame_rate_base = 1;
    sys->es_video = es_out_Add( demux->out, &v );

    /* Audio ES (optional) */
    if( sys->fd_audio >= 0 )
    {
        es_format_t a;
        es_format_Init( &a, AUDIO_ES, VLC_CODEC_S16L );
        a.audio.i_format            = VLC_CODEC_S16L;
        a.audio.i_rate              = sys->audio_rate;
        a.audio.i_channels          = 2;
        a.audio.i_physical_channels = AOUT_CHANS_STEREO;
        a.audio.i_bitspersample     = 16;
        a.audio.i_blockalign        = 4;   /* 2ch * 2 bytes */
        a.audio.i_bytes_per_frame   = 4;
        a.audio.i_frame_length      = 1;
        sys->es_audio = es_out_Add( demux->out, &a );
    }

    /* VLC requires a PCR reset before the first PCR/Send to establish the
     * clock. Without it the first one or two frames get dropped with a
     * "could not convert timestamp 0" message. */
    es_out_Control( demux->out, ES_OUT_RESET_PCR );

    sys->es_announced = true;
    msg_Info( demux, "U64 %s: video %dx%d @ %dHz (SAR %d:%d), audio %s",
              sys->mode == U64S_MODE_NTSC ? "NTSC" : "PAL",
              sys->width, sys->height, sys->fps, sar_num, sar_den,
              sys->fd_audio >= 0 ? "S16LE 2ch enabled" : "disabled" );
}

/* ---- PCR helper (monotonic across streams) ----------------------------- */

static void u64s_update_pcr( demux_t *demux, int64_t pts )
{
    demux_sys_t *sys = demux->p_sys;
    if( pts > sys->last_pcr )
    {
        sys->last_pcr = pts;
        es_out_Control( demux->out, ES_OUT_SET_PCR, pts );
    }
}

/* ---- Frame decode ------------------------------------------------------- */

static void u64s_blit_packet( demux_sys_t *sys,
                              const uint8_t *payload,
                              int y, int n_lines )
{
    if( y < 0 || y + n_lines > sys->height )
        return;

    const int W = sys->width;
    const int half = W / 2;

    for( int row = 0; row < n_lines; ++row )
    {
        const uint8_t *src_row = payload + row * half;
        uint8_t *dst_row = sys->frame_rgba + (y + row) * W * 4;
        for( int xb = 0; xb < half; ++xb )
        {
            uint8_t b = src_row[xb];
            uint32_t c0 = u64s_palette[ b & 0x0F ];
            uint32_t c1 = u64s_palette[ (b >> 4) & 0x0F ];
            uint8_t *p0 = dst_row + xb * 8;
            p0[0] = (c0 >> 16) & 0xFF;
            p0[1] = (c0 >>  8) & 0xFF;
            p0[2] =  c0        & 0xFF;
            p0[3] = 0xFF;
            p0[4] = (c1 >> 16) & 0xFF;
            p0[5] = (c1 >>  8) & 0xFF;
            p0[6] =  c1        & 0xFF;
            p0[7] = 0xFF;
        }
    }
}

static int u64s_emit_frame( demux_t *demux )
{
    demux_sys_t *sys = demux->p_sys;
    u64s_announce_es( demux );
    if( sys->es_video == NULL )
        return VLC_EGENERIC;

    const size_t bytes = (size_t)sys->width * (size_t)sys->height * 4u;
    block_t *blk = block_Alloc( bytes );
    if( unlikely( blk == NULL ) )
        return VLC_ENOMEM;
    memcpy( blk->p_buffer, sys->frame_rgba, bytes );

    int64_t now = u64s_tick_now();
    if( sys->v_t0_us == 0 )
        sys->v_t0_us = now;

    int64_t pts = sys->v_t0_us
                + (int64_t)sys->v_frames * (int64_t)CLOCK_FREQ / sys->fps;
    blk->i_pts = pts;
    blk->i_dts = pts;

    /* PCR follows current wall-clock, NOT the per-block PTS. Otherwise
     * draining a burst of packets in one Demux call advances PCR much
     * faster than wall-clock, and VLC's input clock fires
     * "ES_OUT_SET_PCR called too late" warnings before recovering with
     * an inflated pts_delay. With PCR=mdate(), the input-clock offset is
     * always ~0 and stream-time PTS converts cleanly to wall-clock. */
    u64s_update_pcr( demux, now );

    es_out_Send( demux->out, sys->es_video, blk );
    sys->v_frames++;
    return VLC_SUCCESS;
}

/* ---- Per-packet handlers ----------------------------------------------- */

static int u64s_handle_video_packet( demux_t *demux, const uint8_t *pkt,
                                     ssize_t n )
{
    demux_sys_t *sys = demux->p_sys;

    if( n < U64S_HDR_SIZE_VIDEO )
    {
        msg_Warn( demux, "short video datagram (%zd bytes), ignoring", n );
        return VLC_DEMUXER_SUCCESS;
    }
    uint16_t seq    = (uint16_t)pkt[0] | ((uint16_t)pkt[1] << 8);
    uint16_t lineno = (uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8);
    uint16_t pxline = (uint16_t)pkt[6] | ((uint16_t)pkt[7] << 8);
    uint8_t  lpp    = pkt[8];
    uint8_t  bpp    = pkt[9];

    bool last_of_frame = (lineno & 0x8000u) != 0;
    int  y = lineno & 0x7FFFu;

    if( pxline != U64S_PIXELS_PER_LN || lpp != U64S_LINES_PER_PKT
                                     || bpp != 4 )
    {
        msg_Warn( demux,
                  "non-U64 video datagram (px=%u, lines=%u, bpp=%u); skip",
                  pxline, lpp, bpp );
        return VLC_DEMUXER_SUCCESS;
    }
    if( n < (ssize_t)(U64S_HDR_SIZE_VIDEO + U64S_PAYLOAD_VIDEO) )
    {
        msg_Warn( demux, "truncated video packet (%zd bytes), skip", n );
        return VLC_DEMUXER_SUCCESS;
    }

    if( sys->v_saw_seq && (uint16_t)(sys->v_last_seq + 1) != seq )
        msg_Dbg( demux, "video udp gap: seq %u -> %u",
                 sys->v_last_seq, seq );
    sys->v_last_seq = seq;
    sys->v_saw_seq  = true;

    /* Auto-detect mode from first complete frame: bit15-set y reveals the
     * height (last 4-line group is at y=h-4). */
    if( !sys->detected && last_of_frame )
    {
        int detected_h = y + U64S_LINES_PER_PKT;
        int new_mode = (detected_h <= U64S_NTSC_HEIGHT)
                     ? U64S_MODE_NTSC : U64S_MODE_PAL;
        u64s_apply_mode( sys, new_mode );
    }

    u64s_blit_packet( sys, pkt + U64S_HDR_SIZE_VIDEO, y, U64S_LINES_PER_PKT );

    if( last_of_frame && sys->detected )
    {
        /* The very first bit15 we see marks the END of a frame whose start
         * we missed — the framebuffer has at most 4 valid lines plus
         * black (zero) elsewhere. Skip emitting it; capture the next
         * full frame between this boundary and the next. */
        if( !sys->v_first_boundary_seen )
        {
            sys->v_first_boundary_seen = true;
        }
        else
        {
            if( u64s_emit_frame( demux ) != VLC_SUCCESS )
                return VLC_DEMUXER_EGENERIC;
            /* Optional: clear framebuffer so dropped packets in the new
             * frame show as black rather than persisting last frame's
             * pixels. */
            if( sys->on_loss_clear )
            {
                memset( sys->frame_rgba, 0,
                        (size_t)sys->width * U64S_FRAMEBUF_HEIGHT * 4u );
            }
        }
    }
    return VLC_DEMUXER_SUCCESS;
}

static int u64s_handle_audio_packet( demux_t *demux, const uint8_t *pkt,
                                     ssize_t n )
{
    demux_sys_t *sys = demux->p_sys;

    if( n < (ssize_t)(U64S_HDR_SIZE_AUDIO + U64S_PAYLOAD_AUDIO) )
    {
        /* Note: U64 audio packets are exactly 770 bytes. Anything else is
         * almost certainly not us; complain loudly the first time only. */
        if( !sys->a_saw_seq )
            msg_Warn( demux, "unexpected audio datagram size %zd, ignoring",
                      n );
        return VLC_DEMUXER_SUCCESS;
    }

    uint16_t seq = (uint16_t)pkt[0] | ((uint16_t)pkt[1] << 8);
    if( sys->a_saw_seq && (uint16_t)(sys->a_last_seq + 1) != seq )
        msg_Dbg( demux, "audio udp gap: seq %u -> %u",
                 sys->a_last_seq, seq );
    sys->a_last_seq = seq;
    sys->a_saw_seq  = true;

    /* Wait until the video side has detected PAL/NTSC so audio_rate is set
     * correctly. Until then, drop audio (a few hundred ms at most). */
    if( !sys->detected )
        return VLC_DEMUXER_SUCCESS;

    u64s_announce_es( demux );
    if( sys->es_audio == NULL )
        return VLC_DEMUXER_SUCCESS;

    block_t *blk = block_Alloc( U64S_PAYLOAD_AUDIO );
    if( unlikely( blk == NULL ) )
        return VLC_DEMUXER_EGENERIC;
    memcpy( blk->p_buffer, pkt + U64S_HDR_SIZE_AUDIO, U64S_PAYLOAD_AUDIO );
    blk->i_nb_samples = U64S_AUDIO_FRAMES_PKT;

    int64_t now = u64s_tick_now();
    if( sys->a_t0_us == 0 )
        sys->a_t0_us = now;

    int64_t pts = sys->a_t0_us
                + (int64_t)sys->a_samples * (int64_t)CLOCK_FREQ
                  / sys->audio_rate;
    blk->i_pts = pts;
    blk->i_dts = pts;
    blk->i_length = (int64_t)U64S_AUDIO_FRAMES_PKT * (int64_t)CLOCK_FREQ
                  / sys->audio_rate;

    /* PCR tracks wall-clock (see comment in u64s_emit_frame). */
    u64s_update_pcr( demux, now );

    es_out_Send( demux->out, sys->es_audio, blk );
    sys->a_samples += U64S_AUDIO_FRAMES_PKT;
    return VLC_DEMUXER_SUCCESS;
}

/* ---- Demux callback ---------------------------------------------------- */

static int Demux( demux_t *demux )
{
    demux_sys_t *sys = demux->p_sys;
    struct pollfd pfd[2];
    int nfds = 0;
    int idx_video = -1, idx_audio = -1;

    if( sys->fd_video >= 0 )
    {
        idx_video = nfds;
        pfd[nfds].fd = sys->fd_video;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if( sys->fd_audio >= 0 )
    {
        idx_audio = nfds;
        pfd[nfds].fd = sys->fd_audio;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if( nfds == 0 )
        return VLC_DEMUXER_EOF;

    /* 100 ms timeout keeps the input thread responsive to shutdown. */
    int rc = poll( pfd, nfds, 100 );
    if( rc < 0 )
    {
        if( errno == EINTR )
            return VLC_DEMUXER_SUCCESS;
        msg_Err( demux, "poll() failed: %s", vlc_strerror_c(errno) );
        return VLC_DEMUXER_EGENERIC;
    }
    if( rc == 0 )
        return VLC_DEMUXER_SUCCESS; /* idle tick */

    uint8_t buf[U64S_PKT_SIZE_VIDEO]; /* video pkt is the larger of the two */

    /* Drain ready sockets fully before returning, capped per fd to keep
     * our caller responsive. Without draining, a slow input thread (or a
     * stutter in the host) lets packets pile up in the kernel queue and
     * pushes our PTSes behind wall-clock — VLC's audio output then sees
     * "playback too late" and flushes. */
    enum { DRAIN_LIMIT = 256 };

    if( idx_video >= 0 && (pfd[idx_video].revents & POLLIN) )
    {
        for( int i = 0; i < DRAIN_LIMIT; ++i )
        {
            ssize_t n = u64s_recv_filtered( demux, sys->fd_video,
                                            buf, sizeof(buf) );
            if( n < 0 )
            {
                if( errno == EAGAIN || errno == EWOULDBLOCK
                                    || errno == EINTR )
                    break;
                msg_Err( demux, "video recvfrom: %s",
                         vlc_strerror_c(errno) );
                break;
            }
            if( n == 0 )
                continue; /* source-filter drop */
            int r = u64s_handle_video_packet( demux, buf, n );
            if( r != VLC_DEMUXER_SUCCESS )
                return r;
        }
    }
    if( idx_audio >= 0 && (pfd[idx_audio].revents & POLLIN) )
    {
        for( int i = 0; i < DRAIN_LIMIT; ++i )
        {
            ssize_t n = u64s_recv_filtered( demux, sys->fd_audio,
                                            buf, sizeof(buf) );
            if( n < 0 )
            {
                if( errno == EAGAIN || errno == EWOULDBLOCK
                                    || errno == EINTR )
                    break;
                msg_Err( demux, "audio recvfrom: %s",
                         vlc_strerror_c(errno) );
                break;
            }
            if( n == 0 )
                continue;
            int r = u64s_handle_audio_packet( demux, buf, n );
            if( r != VLC_DEMUXER_SUCCESS )
                return r;
        }
    }
    return VLC_DEMUXER_SUCCESS;
}

/* ---- Control callback -------------------------------------------------- */

static int Control( demux_t *demux, int query, va_list args )
{
    demux_sys_t *sys = demux->p_sys;

    switch( query )
    {
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
        {
            bool *b = va_arg(args, bool *);
            *b = false;
            return VLC_SUCCESS;
        }
        case DEMUX_GET_PTS_DELAY:
        {
            int64_t *d = va_arg(args, int64_t *);
            /* Live UDP streams need real buffering slack between PTS and
             * playback; 20 ms (one frame) is too tight and any kernel-queue
             * jitter makes the audio output flag "playback too late" and
             * flush buffers. 300 ms is the conventional default. */
            *d = INT64_C(300000);
            return VLC_SUCCESS;
        }
        case DEMUX_GET_TIME:
        {
            int64_t *t = va_arg(args, int64_t *);
            int fps = sys->fps > 0 ? sys->fps : U64S_PAL_FPS;
            *t = sys->v_frames
               ? (int64_t)sys->v_frames * CLOCK_FREQ / fps
               : 0;
            return VLC_SUCCESS;
        }
        case DEMUX_GET_LENGTH:
        {
            int64_t *l = va_arg(args, int64_t *);
            *l = 0;
            return VLC_SUCCESS;
        }
        default:
            return VLC_EGENERIC;
    }
}

/* ---- URL helpers ------------------------------------------------------- */

/* Parse an `[mcast-group]@[bind-host]:port[?key=value&...]` location.
 * Either side of `@` may be empty. Examples:
 *
 *   u64://239.0.1.64@:11000        group=239.0.1.64, bind=*, port=11000
 *   u64://239.0.1.64@192.168.2.10:11000   group + specific local interface
 *   u64://@:11000                  group=none, bind=*  (plain unicast)
 *   u64://192.168.2.10:11000       no group, bind=192.168.2.10
 *   u64://:11000                   no group, bind=*
 *   u64://@:11000?source=192.168.2.64    embed source filter in URL
 *
 * Recognised query keys (override their --u64stream-* counterparts):
 *   source=IP
 *
 * The `--u64stream-port` option overrides the port if the URL omits it.
 *
 * Caller frees *out_group, *out_bind, and *out_query_source.
 */
static void u64s_parse_url( demux_t *demux,
                            char **out_group, char **out_bind, int *out_port,
                            char **out_query_source )
{
    *out_group         = NULL;
    *out_bind          = NULL;
    *out_query_source  = NULL;
    *out_port          = (int)var_InheritInteger( demux,
                                                  U64S_CFG_PREFIX "port" );

    const char *loc = demux->psz_location;
    if( loc == NULL || *loc == '\0' )
        return;

    /* Split off the query string (after '?'). */
    const char *qs = strchr( loc, '?' );
    if( qs != NULL )
    {
        const char *q = qs + 1;
        while( *q != '\0' )
        {
            const char *amp = strchr( q, '&' );
            size_t pair_len = amp ? (size_t)(amp - q) : strlen( q );
            const char *eq = memchr( q, '=', pair_len );
            if( eq != NULL )
            {
                size_t klen = (size_t)(eq - q);
                size_t vlen = pair_len - klen - 1;
                if( klen == 6 && strncmp( q, "source", 6 ) == 0 && vlen > 0 )
                {
                    free( *out_query_source );
                    *out_query_source = strndup( eq + 1, vlen );
                }
                /* future: add &mode=, &audio=off, etc. here. */
            }
            if( amp == NULL ) break;
            q = amp + 1;
        }
    }

    size_t loc_len = qs ? (size_t)(qs - loc) : strlen( loc );
    /* Strip a trailing slash if present (before the query). */
    if( loc_len > 0 && loc[loc_len - 1] == '/' )
        loc_len--;
    char *work = strndup( loc, loc_len );
    if( work == NULL )
        return;

    /* Split on the LAST '@'. Multicast group is to the left. */
    char *bindpart = work;
    char *grouppart = NULL;
    char *at = strrchr( work, '@' );
    if( at != NULL )
    {
        *at = '\0';
        grouppart = work;        /* may be empty */
        bindpart  = at + 1;      /* may be empty */
    }

    /* Parse "[bind]:port" — the colon must be the last one (so [::]:port
     * IPv6 literals would still work if/when we ever need them). */
    char *colon = strrchr( bindpart, ':' );
    if( colon != NULL )
    {
        *colon = '\0';
        const char *port_str = colon + 1;
        if( *port_str != '\0' )
        {
            char *end = NULL;
            long p = strtol( port_str, &end, 10 );
            if( end != port_str && p > 0 && p < 65536 )
                *out_port = (int)p;
        }
    }

    /* Strip [] from IPv6 literal, if present, in either part. */
    char *strip_brackets = NULL;
    for( int pass = 0; pass < 2; pass++ )
    {
        char **pp = (pass == 0) ? &grouppart : &bindpart;
        if( *pp == NULL ) continue;
        size_t L = strlen(*pp);
        if( L >= 2 && (*pp)[0] == '[' && (*pp)[L-1] == ']' )
        {
            (*pp)[L-1] = '\0';
            (*pp)++;
        }
    }
    (void)strip_brackets;

    if( grouppart != NULL && *grouppart != '\0' )
        *out_group = strdup( grouppart );
    if( bindpart != NULL && *bindpart != '\0' )
        *out_bind  = strdup( bindpart );

    free( work );
}

/* ---- Telnet auto-start ------------------------------------------------- */

/* The U64 firmware exposes its on-screen menu over telnet (TCP/23). Sending
 * the keystroke sequence below toggles the "Audio/Video Stream" entry in
 * the F5 menu, which starts the UDP stream. Layout-dependent and FRAGILE,
 * but matches the published firmware default and what u64view (WTFPL) uses
 * for the same purpose. */
static const uint8_t U64S_START_SEQ[] = {
    0x1B, 0x5B, 0x31, 0x35, 0x7E,  /* F5 (CSI 15~) */
    0x1B, 0x5B, 0x42,              /* arrow down */
    0x1B, 0x5B, 0x42,
    0x1B, 0x5B, 0x42,
    0x1B, 0x5B, 0x42,
    0x1B, 0x5B, 0x42,
    0x1B, 0x5B, 0x42,
    0x1B, 0x5B, 0x42,
    0x1B, 0x5B, 0x42,              /* 8 arrow-downs total */
    0x0D, 0x00,                    /* Enter */
    0x0D, 0x00,
    0x0D, 0x00,
};

/* Parse "host" or "host:port" into its components. Default port is 23. */
static void u64s_split_host_port( const char *spec, char **host, int *port )
{
    *host = NULL;
    *port = 23;
    if( spec == NULL || *spec == '\0' )
        return;
    char *colon = strrchr( spec, ':' );
    if( colon != NULL && colon != spec )
    {
        *host = strndup( spec, (size_t)(colon - spec) );
        char *end = NULL;
        long p = strtol( colon + 1, &end, 10 );
        if( end != colon + 1 && p > 0 && p < 65536 )
            *port = (int)p;
    }
    else
    {
        *host = strdup( spec );
    }
}

/* Send the U64 start-stream keystroke sequence over a fresh TCP connection.
 * Best-effort: any failure just logs a warning. */
static void u64s_send_start_sequence( demux_t *demux, const char *spec )
{
    char *host = NULL;
    int port = 23;
    u64s_split_host_port( spec, &host, &port );
    if( host == NULL || *host == '\0' )
    {
        free( host );
        return;
    }
    msg_Info( demux, "sending stream-start sequence to %s:%d", host, port );

    int fd = net_ConnectTCP( demux, host, port );
    if( fd < 0 )
    {
        msg_Warn( demux, "cannot connect to control host %s:%d", host, port );
        free( host );
        return;
    }

    /* The U64 telnet endpoint processes one byte at a time slowly.
     * Mimic u64view's pacing: short initial pause, then 1-byte sends. */
    usleep( 10 * 1000 );
    for( size_t i = 0; i < sizeof(U64S_START_SEQ); ++i )
    {
        usleep( 1 * 1000 );
        ssize_t n = net_Write( demux, fd, &U64S_START_SEQ[i], 1 );
        if( n <= 0 )
        {
            msg_Warn( demux,
                      "control-host write failed at byte %zu", i );
            break;
        }
    }
    net_Close( fd );
    free( host );
}

/* ---- Open / Close ------------------------------------------------------ */

static int Open( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;

    if( demux->psz_access == NULL ||
        ( strcmp(demux->psz_access, "u64") != 0 &&
          strcmp(demux->psz_access, "u64stream") != 0 ) )
    {
        return VLC_EGENERIC;
    }

    /* Reject preparse / metadata-extraction passes. */
    if( demux->b_preparsing )
        return VLC_EGENERIC;

    /* Global one-instance lock. See u64s_live_instances above. */
    int prev = atomic_fetch_add( &u64s_live_instances, 1 );
    if( prev > 0 )
    {
        atomic_fetch_sub( &u64s_live_instances, 1 );
        msg_Dbg( demux, "another u64stream instance is live; refusing" );
        return VLC_EGENERIC;
    }

    demux_sys_t *sys = calloc( 1, sizeof(*sys) );
    if( unlikely( sys == NULL ) )
        return VLC_ENOMEM;
    sys->fd_video = -1;
    sys->fd_audio = -1;
    sys->last_pcr = VLC_TS_INVALID;

    /* ---- Read options ---- */
    int requested_mode = (int)var_InheritInteger( demux,
                            U64S_CFG_PREFIX "mode" );
    if( requested_mode == U64S_MODE_PAL || requested_mode == U64S_MODE_NTSC )
        u64s_apply_mode( sys, requested_mode );
    else
    {
        /* Stage defaults so DEMUX_GET_PTS_DELAY etc. have sane values
         * before the first frame arrives; auto-detect overwrites them. */
        sys->mode       = U64S_MODE_AUTO;
        sys->detected   = false;
        sys->height     = U64S_PAL_HEIGHT;
        sys->fps        = U64S_PAL_FPS;
        sys->audio_rate = U64S_PAL_AUDIO_RATE;
    }
    sys->width = U64S_PIXELS_PER_LN;

    sys->sar_num_override = (int)var_InheritInteger( demux,
                                U64S_CFG_PREFIX "sar-num" );
    sys->sar_den_override = (int)var_InheritInteger( demux,
                                U64S_CFG_PREFIX "sar-den" );
    sys->on_loss_clear = var_InheritInteger( demux,
                            U64S_CFG_PREFIX "on-loss" ) != 0;

    /* Always allocate the larger framebuffer so we don't reallocate after
     * mode auto-detection. */
    sys->frame_rgba = calloc( 1,
        (size_t)sys->width * U64S_FRAMEBUF_HEIGHT * 4u );
    if( unlikely( sys->frame_rgba == NULL ) )
    {
        free( sys );
        return VLC_ENOMEM;
    }

    char *group = NULL;
    char *bindhost = NULL;
    char *url_source = NULL;
    int port_video = U64S_DEFAULT_PORT_VIDEO;
    u64s_parse_url( demux, &group, &bindhost, &port_video, &url_source );

    /* Resolve source filter: URL query overrides --u64stream-source. */
    char *src_str = url_source;
    if( src_str == NULL )
        src_str = var_InheritString( demux, U64S_CFG_PREFIX "source" );
    if( src_str != NULL && *src_str != '\0' )
    {
        if( inet_pton( AF_INET, src_str, &sys->src ) == 1 )
        {
            sys->filter_src = true;
            msg_Info( demux, "source filter active: only accepting "
                             "packets from %s", src_str );
        }
        else
        {
            msg_Warn( demux, "ignoring invalid source IP '%s'", src_str );
        }
    }
    free( src_str );

    bool no_video = var_InheritBool( demux, U64S_CFG_PREFIX "no-video" );
    bool no_audio = var_InheritBool( demux, U64S_CFG_PREFIX "no-audio" );

    if( no_video && no_audio )
    {
        msg_Err( demux, "both video and audio disabled; nothing to do" );
        free( group );
        free( bindhost );
        free( sys->frame_rgba );
        free( sys );
        atomic_fetch_sub( &u64s_live_instances, 1 );
        return VLC_EGENERIC;
    }

    if( !no_video )
    {
        sys->fd_video = u64s_open_socket( demux, bindhost, group,
                                          port_video, "video" );
        if( sys->fd_video < 0 )
        {
            free( group );
            free( bindhost );
            free( sys->frame_rgba );
            free( sys );
            atomic_fetch_sub( &u64s_live_instances, 1 );
            return VLC_EGENERIC;
        }
    }
    else
    {
        msg_Info( demux, "video disabled by --u64stream-no-video" );
        /* No video means no first-frame mode auto-detection; if the user
         * didn't pin a mode explicitly we default to PAL so audio can
         * start flowing. Mark detection complete either way. */
        if( !sys->detected )
            u64s_apply_mode( sys, U64S_MODE_PAL );
    }
    if( !no_audio )
    {
        int port_audio = (int)var_InheritInteger( demux,
                            U64S_CFG_PREFIX "audio-port" );
        if( port_audio <= 0 )
            port_audio = port_video + 1;

        /* Audio multicast group: explicit override, else auto-derive from
         * video group by incrementing the last IPv4 octet (matches the
         * U64's default, e.g. 239.0.1.64 -> 239.0.1.65). For unicast
         * (no group), audio is unicast on the same bind. */
        char *audio_group_opt = var_InheritString( demux,
                                    U64S_CFG_PREFIX "audio-group" );
        char *audio_group = NULL;
        if( audio_group_opt != NULL && *audio_group_opt != '\0' )
        {
            audio_group = strdup( audio_group_opt );
        }
        else if( group != NULL )
        {
            struct in_addr g;
            if( inet_pton( AF_INET, group, &g ) == 1 )
            {
                /* Increment the last octet (mod 256). */
                uint32_t h = ntohl( g.s_addr );
                h = (h & 0xFFFFFF00u) | ((h + 1) & 0xFFu);
                g.s_addr = htonl( h );
                char buf[INET_ADDRSTRLEN];
                if( inet_ntop( AF_INET, &g, buf, sizeof(buf) ) != NULL )
                    audio_group = strdup( buf );
            }
            else
            {
                /* Non-IPv4 group; just reuse it for audio. */
                audio_group = strdup( group );
            }
        }
        free( audio_group_opt );

        sys->fd_audio = u64s_open_socket( demux, bindhost, audio_group,
                                          port_audio, "audio" );
        free( audio_group );
        if( sys->fd_audio < 0 )
            msg_Warn( demux, "audio socket failed to open; video-only" );
    }
    free( group );
    free( bindhost );

    demux->p_sys      = sys;
    demux->pf_demux   = Demux;
    demux->pf_control = Control;

    /* If asked, ping the U64's telnet menu to start streaming. Done last
     * so the UDP sockets are already listening when packets begin. */
    char *ctl_host = var_InheritString( demux,
                        U64S_CFG_PREFIX "control-host" );
    if( ctl_host != NULL && *ctl_host != '\0' )
        u64s_send_start_sequence( demux, ctl_host );
    free( ctl_host );

    return VLC_SUCCESS;
}

static void Close( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;
    demux_sys_t *sys = demux->p_sys;
    if( sys == NULL )
        return;
    if( sys->fd_video >= 0 ) net_Close( sys->fd_video );
    if( sys->fd_audio >= 0 ) net_Close( sys->fd_audio );
    free( sys->frame_rgba );
    free( sys );
    atomic_fetch_sub( &u64s_live_instances, 1 );
}
