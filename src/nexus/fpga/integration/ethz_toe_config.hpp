#pragma once

// Force-include this file in every upstream TOE translation unit and the adapter.
// Changing one setting requires rebuilding the complete HLS IP and its integration.
#if defined(FNS_DATA_WIDTH) && FNS_DATA_WIDTH != 8
#error "The TOE adapter requires FNS_DATA_WIDTH=8 bytes"
#endif
#ifndef FNS_DATA_WIDTH
#define FNS_DATA_WIDTH 8
#endif

#if defined(TCP_NODELAY) && TCP_NODELAY != 1
#error "The TOE adapter requires TCP_NODELAY=1"
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#if defined(TCP_MSS) && TCP_MSS != 1460
#error "The TOE adapter requires TCP_MSS=1460 bytes"
#endif
#ifndef TCP_MSS
#define TCP_MSS 1460
#endif

#if defined(TCP_STACK_MAX_SESSIONS) && TCP_STACK_MAX_SESSIONS != 64
#error "The TOE adapter requires TCP_STACK_MAX_SESSIONS=64"
#endif
#ifndef TCP_STACK_MAX_SESSIONS
#define TCP_STACK_MAX_SESSIONS 64
#endif

#if defined(RX_DDR_BYPASS) && RX_DDR_BYPASS != 0
#error "The TOE adapter requires RX_DDR_BYPASS=0"
#endif
#ifndef RX_DDR_BYPASS
#define RX_DDR_BYPASS 0
#endif

#if defined(FAST_RETRANSMIT) && FAST_RETRANSMIT != 1
#error "The TOE adapter requires FAST_RETRANSMIT=1"
#endif
#ifndef FAST_RETRANSMIT
#define FAST_RETRANSMIT 1
#endif

#if defined(WINDOW_SCALE) && WINDOW_SCALE != 1
#error "The TOE adapter requires WINDOW_SCALE=1"
#endif
#ifndef WINDOW_SCALE
#define WINDOW_SCALE 1
#endif

// Required by the shared upstream configuration header; this IP contains no RoCE.
#if defined(FNS_ROCE_STACK_MAX_QPS) && FNS_ROCE_STACK_MAX_QPS != 1
#error "The TOE adapter requires FNS_ROCE_STACK_MAX_QPS=1"
#endif
#ifndef FNS_ROCE_STACK_MAX_QPS
#define FNS_ROCE_STACK_MAX_QPS 1
#endif
