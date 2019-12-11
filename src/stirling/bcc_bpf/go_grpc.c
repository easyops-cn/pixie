// LINT_C_FILE: Do not remove this line. It ensures cpplint treats this as a C file.

#include "src/stirling/bcc_bpf_interface/go_grpc_types.h"

#define HEADER_COUNT 64

BPF_PERF_OUTPUT(go_grpc_header_events);
// Key: TGID
// Value: Symbol addresses for the binary with that TGID.
BPF_HASH(symaddrs_map, uint32_t, struct conn_symaddrs_t);

// From golang source:
// //A HeaderField is a name-value pair. Both the name and value are
// // treated as opaque sequences of octets.
// type HeaderField struct {
//   Name, Value string
//
//   // Sensitive means that this header field should never be
//   // indexed.
//   Sensitive bool
// }
struct HPackHeaderField {
  struct gostring name;
  struct gostring value;
  bool sensitive;
};

// From golang source:
// type FD struct {
//   fdmu fdMutex
//   // fdMutex is 16 bytes.
//   type fdMutex struct {
//     state uint64
//     rsema uint32
//     wsema uint32
//   }
//   Sysfd int
//   ...
// }
struct FD {
  uint64_t fdmu0;
  uint64_t fdmu1;
  int64_t sysfd;
};

static __inline int32_t conn_fd2(const void* framer_ptr) {
  // This function accesses one of the following:
  //   f.w.conn.conn.conn.fd.pfd.Sysfd
  //   f.w.conn.conn.fd.pfd.Sysfd
  //   f.w.conn.fd.pfd.Sysfd
  // The right one to use depends on the context (e.g. whether the connection uses TLS or not).

  // (gdb) x ($sp+8)
  // 0xc000069e48:  0x000000c0001560e0
  // (gdb) x/2gx (0x000000c0001560e0+112)
  // 0xc000156150:  0x0000000000b2b1e0  0x000000c0000caf00
  // (gdb) x 0x0000000000b2b1e0
  // 0xb2b1e0 <go.itab.*google.golang.org/grpc/internal/transport.bufWriter,io.Writer>:
  // 0x00000000009c9400 (gdb) x/2gx (0x000000c0000caf00+40) 0xc0000caf28:  0x0000000000b3bf60
  // 0x000000c00000ec20 (gdb) x 0x0000000000b3bf60 0xb3bf60
  // <go.itab.*google.golang.org/grpc/credentials/internal.syscallConn,net.Conn>: 0x00000000009f66c0
  // (gdb) x/2gx 0x000000c00000ec20
  // 0xc00000ec20:  0x0000000000b3bea0  0x000000c000059180
  // (gdb) x 0x0000000000b3bea0
  // 0xb3bea0 <go.itab.*crypto/tls.Conn,net.Conn>:  0x00000000009f66c0
  // (gdb) x/2gx 0x000000c000059180
  // 0xc000059180:  0x0000000000b3c020  0x000000c000010048
  // (gdb) x 0x0000000000b3c020
  // 0xb3c020 <go.itab.*net.TCPConn,net.Conn>:  0x00000000009f66c0

  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t tgid = current_pid_tgid >> 32;
  struct conn_symaddrs_t* symaddrs = symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  // From llvm-dwarfdump -n w ./client
  // 0x00070b39: DW_TAG_member
  //              DW_AT_name  ("w")
  //              DW_AT_data_member_location  (112)
  //              DW_AT_type  (0x000000000004a883 "io.Writer")
  //              DW_AT_unknown_2903  (0x00)
  const int kFramerIOWriterOffset = 112;
  struct go_interface io_writer_interface;
  bpf_probe_read(&io_writer_interface, sizeof(io_writer_interface),
                 framer_ptr + kFramerIOWriterOffset);

  const int kIOWriterConnOffset = 40;
  struct go_interface conn_interface;
  bpf_probe_read(&conn_interface, sizeof(conn_interface),
                 io_writer_interface.ptr + kIOWriterConnOffset);

  if (conn_interface.type == symaddrs->syscall_conn) {
    const int kSyscallConnConnOffset = 0;
    bpf_probe_read(&conn_interface, sizeof(conn_interface),
                   conn_interface.ptr + kSyscallConnConnOffset);
  }

  if (conn_interface.type == symaddrs->tls_conn) {
    const int kTLSConnConnOffset = 0;
    bpf_probe_read(&conn_interface, sizeof(conn_interface),
                   conn_interface.ptr + kTLSConnConnOffset);
  }

  if (conn_interface.type != symaddrs->tcp_conn) {
    return 0;
  }

  void* fd_ptr;
  bpf_probe_read(&fd_ptr, sizeof(fd_ptr), conn_interface.ptr);

  struct FD fd;
  const int kFDOffset = 0;
  // fd = *(struct FD*)(inner_conn_interface.ptr + kFDOffset);
  bpf_probe_read(&fd, sizeof(fd), fd_ptr + kFDOffset);

  return fd.sysfd;
}

// TODO(yzhao): Replace this with conn_fd2. conn_fd2() requires additional data from user-space;
// so decide to do this so to limit the changes needed.
static __inline int32_t conn_fd(const void* conn_ptr) {
  // conn is an interface of net.Conn. The data is the 2nd pointer, which is after the 1st pointer.
  // 'data' points to a *net.TCPConn object. So we need an additional dereferencing to get the
  // pointer to the net.TCPConn object.
  const int kDataFieldOffset = 8;
  const void* framer_writer_conn_data_ptr = *(const void**)(conn_ptr + kDataFieldOffset);

  // net.TCPConn is equivalent to net.conn, which includes a single field fd *net.netFD.
  // To get the address of net.netFD, we need to dereference the ptr here.
  const void* framer_writer_conn_data_fd_ptr = *(const void**)(framer_writer_conn_data_ptr);

  // The Sysfd is the 2nd field of the fd field of net.netFD. The first field is 16 bytes.
  // Sysfd is golang int type, which is platform dependent. It probably is safer to read as int32_t,
  // which avoids reading beyond the valid range; and it's probably impossible to have file
  // descriptor that is beyond the int32_t range.
  const int kFDFieldOffset = 16;
  const int32_t fd = *(int32_t*)(framer_writer_conn_data_fd_ptr + kFDFieldOffset);
  return fd;
}

static __inline void fill_header_field(struct go_grpc_http2_header_event_t* event,
                                       const struct HPackHeaderField* user_space_ptr) {
  struct HPackHeaderField field;
  bpf_probe_read(&field, sizeof(struct HPackHeaderField), user_space_ptr);

  event->name.size = BPF_LEN_CAP(field.name.len, HEADER_FIELD_STR_SIZE);
  bpf_probe_read(event->name.msg, event->name.size, field.name.ptr);

  event->value.size = BPF_LEN_CAP(field.value.len, HEADER_FIELD_STR_SIZE);
  bpf_probe_read(event->value.msg, event->value.size, field.value.ptr);
}

static __inline void fill_probe_info(struct ProbeInfo* probe_info) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  probe_info->timestamp_ns = bpf_ktime_get_ns();
  probe_info->tgid = current_pid_tgid >> 32;
  probe_info->tid = current_pid_tgid & 0xffffffff;
}

// Probes (*loopyWriter).writeHeader(uint32, bool, []hpack.HeaderField, func()) inside gRPC-go,
// which writes HTTP2 headers to the server.
int probe_loopy_writer_write_header(struct pt_regs* ctx) {
  // The following code tries to extract the file descriptor held by the loopyWriter object, when
  // probing the (*loopyWriter).writeHeader() function call. The whole structure looks like:
  // type loopyWriter struct {
  //   ...  // 40 bytes offset
  //   framer *framer
  //   type framer struct {
  //     writer *bufWriter
  //     type bufWriter struct {
  //       ...  // 40 bytes offset
  //       conn net.Conn
  //       type net.Conn interface {
  //         ...  // 8 bytes offset
  //         data  // An pointer to *net.TCPConn, which implements the net.Conn interface.
  //         type TCPConn struct {
  //           conn  // conn is embedded inside TCPConn, which is defined as follows.
  //           type conn struct {
  //             fd *netFD
  //             type netFD struct {
  //               pfd poll.FD
  //               type FD struct {
  //                 ...  // 16 bytes offset
  //                 Sysfd int
  //               }
  //             }
  //           }
  //         }
  //       }
  //     }
  //   }
  // }
  const void* sp = (const void*)PT_REGS_SP(ctx);
  const int kLoopyWriterParamOffset = 8;
  const void* loopy_writer_ptr = *(const void**)(sp + kLoopyWriterParamOffset);

  const int kFramerFieldOffset = 40;
  const void* framer_ptr = *(const void**)(loopy_writer_ptr + kFramerFieldOffset);
  const void* framer_writer_ptr = *(const void**)framer_ptr;

  const int kConnFieldOffset = 40;
  const void* framer_writer_conn_ptr = framer_writer_ptr + kConnFieldOffset;
  const int32_t fd = conn_fd(framer_writer_conn_ptr);

  const int kStreamIDParamOffset = 16;
  const uint32_t stream_id = *(uint32_t*)(sp + kStreamIDParamOffset);

  struct go_grpc_http2_header_event_t event = {
      .type = kGRPCWriteHeader,
      .fd = fd,
      .stream_id = stream_id,
  };
  fill_probe_info(&event.entry_probe);

  const int kHeaderFieldSliceParamOffset = 24;
  const struct go_ptr_array fields =
      *(const struct go_ptr_array*)(sp + kHeaderFieldSliceParamOffset);
  const struct HPackHeaderField* fields_ptr = fields.ptr;

  // Using 'int i' below seems prevent loop getting rolled.
  // TODO(yzhao): Investigate and fix.
#pragma unroll
  for (unsigned int i = 0, pos = 0; i < LOOP_LIMIT && i < fields.len; ++i) {
    fill_header_field(&event, fields_ptr + i);
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }

  return 0;
}

// Probes (*http2Client).operateHeaders(*http2.MetaHeadersFrame) inside gRPC-go, which processes
// HTTP2 headers of the received responses.
int probe_http2_client_operate_headers(struct pt_regs* ctx) {
  const void* sp = (const void*)PT_REGS_SP(ctx);

  const void* http2_client_ptr = *(const void**)(sp + 8);
  const int kHTTP2ClientConnFieldOffset = 64;
  const void* http2_client_conn_ptr = http2_client_ptr + kHTTP2ClientConnFieldOffset;
  const uint32_t fd = conn_fd(http2_client_conn_ptr);

  const int kFieldsParamOffset = 16;
  const void* frame_ptr = *(const void**)(sp + kFieldsParamOffset);

  const void* frame_header_ptr = *(const void**)frame_ptr;
  const int kStreamIDOffset = 8;
  const uint32_t stream_id = *(const uint32_t*)(frame_header_ptr + kStreamIDOffset);

  const int kFieldsOffset = 8;
  const struct go_ptr_array fields = *(const struct go_ptr_array*)(frame_ptr + kFieldsOffset);
  const struct HPackHeaderField* fields_ptr = fields.ptr;

  // TODO(yzhao): We saw some arbitrary large slices received by operateHeaders(), it's not clear
  // what conditions result into them.
  if (fields.len > 100 || fields.len <= 0 || fields.cap <= 0) {
    return 0;
  }

  struct go_grpc_http2_header_event_t event = {
      .type = kGRPCOperateHeaders,
      .fd = fd,
      .stream_id = stream_id,
  };
  fill_probe_info(&event.entry_probe);

  // Using 'int i' below seems prevent loop getting rolled.
  // TODO(yzhao): Investigate and fix.
#pragma unroll
  for (unsigned int i = 0, pos = 0; i < HEADER_COUNT && i < fields.len; ++i) {
    fill_header_field(&event, fields_ptr + i);
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }

  return 0;
}
