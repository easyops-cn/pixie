#include "src/stirling/source_connectors/socket_tracer/uprobe_manager.h"

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <map>

#include "src/common/base/base.h"
#include "src/common/base/utils.h"
#include "src/common/fs/fs_wrapper.h"
#include "src/stirling/bpf_tools/macros.h"
#include "src/stirling/obj_tools/dwarf_tools.h"
#include "src/stirling/source_connectors/socket_tracer/bcc_bpf_intf/symaddrs.h"
#include "src/stirling/source_connectors/socket_tracer/uprobe_symaddrs.h"
#include "src/stirling/utils/proc_path_tools.h"

DEFINE_bool(stirling_rescan_for_dlopen, false,
            "If enabled, Stirling will use mmap tracing information to rescan binaries for delay "
            "loaded libraries like OpenSSL");

namespace pl {
namespace stirling {

using ::pl::stirling::obj_tools::DwarfReader;
using ::pl::stirling::obj_tools::ElfReader;

UProbeManager::UProbeManager(bpf_tools::BCCWrapper* bcc) : bcc_(bcc) {
  proc_parser_ = std::make_unique<system::ProcParser>(system::Config::GetInstance());
}

void UProbeManager::Init(bool enable_http2_tracing, bool disable_self_probing) {
  cfg_enable_http2_tracing_ = enable_http2_tracing;
  cfg_disable_self_probing_ = disable_self_probing;

  openssl_symaddrs_map_ = std::make_unique<ebpf::BPFHashTable<uint32_t, struct openssl_symaddrs_t>>(
      bcc_->bpf().get_hash_table<uint32_t, struct openssl_symaddrs_t>("openssl_symaddrs_map"));
  go_common_symaddrs_map_ =
      std::make_unique<ebpf::BPFHashTable<uint32_t, struct go_common_symaddrs_t>>(
          bcc_->bpf().get_hash_table<uint32_t, struct go_common_symaddrs_t>(
              "go_common_symaddrs_map"));
  go_http2_symaddrs_map_ =
      std::make_unique<ebpf::BPFHashTable<uint32_t, struct go_http2_symaddrs_t>>(
          bcc_->bpf().get_hash_table<uint32_t, struct go_http2_symaddrs_t>("http2_symaddrs_map"));
  go_tls_symaddrs_map_ = std::make_unique<ebpf::BPFHashTable<uint32_t, struct go_tls_symaddrs_t>>(
      bcc_->bpf().get_hash_table<uint32_t, struct go_tls_symaddrs_t>("go_tls_symaddrs_map"));

  mmap_events_ = std::make_unique<ebpf::BPFHashTable<upid_t, bool>>(
      bcc_->bpf().get_hash_table<upid_t, bool>("mmap_events"));
}

StatusOr<int> UProbeManager::AttachUProbeTmpl(const ArrayView<UProbeTmpl>& probe_tmpls,
                                              const std::string& binary,
                                              obj_tools::ElfReader* elf_reader) {
  using bpf_tools::BPFProbeAttachType;

  int uprobe_count = 0;
  for (const auto& tmpl : probe_tmpls) {
    bpf_tools::UProbeSpec spec = {binary,
                                  {},
                                  0,
                                  bpf_tools::UProbeSpec::kDefaultPID,
                                  tmpl.attach_type,
                                  std::string(tmpl.probe_fn)};

    StatusOr<std::vector<ElfReader::SymbolInfo>> symbol_infos_status =
        elf_reader->ListFuncSymbols(tmpl.symbol, tmpl.match_type);
    if (!symbol_infos_status.ok()) {
      VLOG(1) << absl::Substitute("Could not list symbols [error=$0]",
                                  symbol_infos_status.ToString());
      continue;
    }
    const std::vector<ElfReader::SymbolInfo>& symbol_infos = symbol_infos_status.ValueOrDie();

    for (const auto& symbol_info : symbol_infos) {
      switch (tmpl.attach_type) {
        case BPFProbeAttachType::kEntry:
        case BPFProbeAttachType::kReturn: {
          spec.symbol = symbol_info.name;
          PL_RETURN_IF_ERROR(bcc_->AttachUProbe(spec));
          ++uprobe_count;
          break;
        }
        case BPFProbeAttachType::kReturnInsts: {
          // TODO(yzhao): The following code that produces multiple UProbeSpec objects cannot be
          // replaced by TransformGolangReturnProbe(), because LLVM and ELFIO defines conflicting
          // symbol: EI_MAG0 appears as enum in include/llvm/BinaryFormat/ELF.h [1] and
          // EI_MAG0 appears as a macro in elfio/elf_types.hpp [2]. And there are many other such
          // symbols as well.
          //
          // [1] https://llvm.org/doxygen/BinaryFormat_2ELF_8h_source.html
          // [2] https://github.com/eth-sri/debin/blob/master/cpp/elfio/elf_types.hpp
          PL_ASSIGN_OR_RETURN(std::vector<uint64_t> ret_inst_addrs,
                              elf_reader->FuncRetInstAddrs(symbol_info));
          for (const uint64_t& addr : ret_inst_addrs) {
            spec.attach_type = BPFProbeAttachType::kEntry;
            spec.address = addr;
            PL_RETURN_IF_ERROR(bcc_->AttachUProbe(spec));
            ++uprobe_count;
          }
          break;
        }
        default:
          LOG(DFATAL) << "Invalid attach type in switch statement.";
      }
    }
  }
  return uprobe_count;
}

namespace {
Status UpdateOpenSSLSymAddrs(
    std::filesystem::path libcrypto_path, uint32_t pid,
    ebpf::BPFHashTable<uint32_t, struct openssl_symaddrs_t>* openssl_symaddrs_map) {
  PL_ASSIGN_OR_RETURN(struct openssl_symaddrs_t symaddrs, OpenSSLSymAddrs(libcrypto_path));

  ebpf::StatusTuple s = openssl_symaddrs_map->update_value(pid, symaddrs);
  LOG_IF(WARNING, s.code() != 0) << absl::StrCat("Could not update openssl_symaddrs_map. Message=",
                                                 s.msg());

  return Status::OK();
}

Status UpdateGoCommonSymAddrs(
    ElfReader* elf_reader, DwarfReader* dwarf_reader, const std::vector<int32_t>& pids,
    ebpf::BPFHashTable<uint32_t, struct go_common_symaddrs_t>* go_common_symaddrs_map) {
  PL_ASSIGN_OR_RETURN(struct go_common_symaddrs_t symaddrs,
                      GoCommonSymAddrs(elf_reader, dwarf_reader));

  for (auto& pid : pids) {
    ebpf::StatusTuple s = go_common_symaddrs_map->update_value(pid, symaddrs);
    LOG_IF(WARNING, s.code() != 0)
        << absl::StrCat("Could not update go_common_symaddrs_map. Message=", s.msg());
  }

  return Status::OK();
}

Status UpdateGoHTTP2SymAddrs(
    ElfReader* elf_reader, DwarfReader* dwarf_reader, const std::vector<int32_t>& pids,
    ebpf::BPFHashTable<uint32_t, struct go_http2_symaddrs_t>* http2_symaddrs_map) {
  PL_ASSIGN_OR_RETURN(struct go_http2_symaddrs_t symaddrs,
                      GoHTTP2SymAddrs(elf_reader, dwarf_reader));

  for (auto& pid : pids) {
    ebpf::StatusTuple s = http2_symaddrs_map->update_value(pid, symaddrs);
    LOG_IF(WARNING, s.code() != 0)
        << absl::StrCat("Could not update http2_symaddrs_map. Message=", s.msg());
  }

  return Status::OK();
}

Status UpdateGoTLSSymAddrs(
    ElfReader* elf_reader, DwarfReader* dwarf_reader, const std::vector<int32_t>& pids,
    ebpf::BPFHashTable<uint32_t, struct go_tls_symaddrs_t>* go_tls_symaddrs_map) {
  PL_ASSIGN_OR_RETURN(struct go_tls_symaddrs_t symaddrs, GoTLSSymAddrs(elf_reader, dwarf_reader));

  for (auto& pid : pids) {
    ebpf::StatusTuple s = go_tls_symaddrs_map->update_value(pid, symaddrs);
    LOG_IF(WARNING, s.code() != 0)
        << absl::StrCat("Could not update go_tls_symaddrs_map. Message=", s.msg());
  }

  return Status::OK();
}
}  // namespace

// TODO(oazizi/yzhao): Should HTTP uprobes use a different set of perf buffers than the kprobes?
// That allows the BPF code and companion user-space code for uprobe & kprobe be separated
// cleanly. For example, right now, enabling uprobe & kprobe simultaneously can crash Stirling,
// because of the mixed & duplicate data events from these 2 sources.
StatusOr<int> UProbeManager::AttachGoHTTP2Probes(
    const std::string& binary, obj_tools::ElfReader* elf_reader,
    obj_tools::DwarfReader* dwarf_reader, const std::vector<int32_t>& pids,
    ebpf::BPFHashTable<uint32_t, struct go_http2_symaddrs_t>* go_http2_symaddrs_map) {
  // Step 1: Update BPF symaddrs for this binary.
  Status s = UpdateGoHTTP2SymAddrs(elf_reader, dwarf_reader, pids, go_http2_symaddrs_map);
  if (!s.ok()) {
    return 0;
  }

  // Step 2: Deploy uprobes on all new binaries.
  auto result = http2_probed_binaries_.insert(binary);
  if (!result.second) {
    // This is not a new binary, so nothing more to do.
    return 0;
  }
  return AttachUProbeTmpl(kHTTP2ProbeTmpls, binary, elf_reader);
}

// Find the paths for some libraries, which may be inside of a container.
// Return those paths as a vector, in the same order that they came in as function arguments.
// e.g. input: lib_names = {"libssl.so.1.1", "libcrypto.so.1.1"}
// output: {"/usr/lib/mount/abc...def/usr/lib/libssl.so.1.1",
// "/usr/lib/mount/abc...def/usr/lib/libcrypto.so.1.1"}
StatusOr<std::vector<std::filesystem::path>> FindLibraryPaths(
    const std::vector<std::string_view>& lib_names, uint32_t pid, system::ProcParser* proc_parser) {
  // TODO(jps): use a mutable map<string, path> as the function argument.
  // i.e. mapping from lib_name to lib_path.
  // This would relieve the caller of the burden of tracking which entry
  // in the vector belonged to which library it wanted to find.

  // TODO(jps/oazizi): Optimization could be to move the fp_resolver up in the call chain, so it is
  // only created once.
  //                   Would involve splitting to Create() and SetMountNamespace().
  PL_ASSIGN_OR_RETURN(std::unique_ptr<FilePathResolver> fp_resolver, FilePathResolver::Create(pid));

  PL_ASSIGN_OR_RETURN(absl::flat_hash_set<std::string> mapped_lib_paths,
                      proc_parser->GetMapPaths(pid));

  // container_libs: final function output.
  // found_vector: tracks the found status of each lib.
  // Initialize the return vector with empty paths,
  // and setup our state to "nothing found yet"
  std::vector<std::filesystem::path> container_libs(lib_names.size());
  std::vector<bool> found_vector(lib_names.size(), false);

  for (const auto& [lib_idx, lib_name] : Enumerate(lib_names)) {
    if (found_vector[lib_idx]) {
      // This lib has already been found,
      // do not search through the mapped lib paths found by GetMapPaths.
      continue;
    }

    for (const auto& mapped_lib_path : mapped_lib_paths) {
      if (absl::EndsWith(mapped_lib_path, lib_name)) {
        // We found a mapped_lib_path that matches to the desired lib_name.
        // First, get the containerized file path using ResolvePath().
        StatusOr<std::filesystem::path> container_lib_status =
            fp_resolver->ResolvePath(mapped_lib_path);

        if (!container_lib_status.ok()) {
          VLOG(1) << absl::Substitute("Unable to resolve $0 path. Message: $1", lib_name,
                                      container_lib_status.msg());
          continue;
        }

        // Assign the resolved path into the output vector at the appropriate index.
        // Update found status,
        // and continue to search current set of mapped libs for next desired lib.
        container_libs[lib_idx] = container_lib_status.ValueOrDie();
        found_vector[lib_idx] = true;
        VLOG(1) << absl::Substitute("Resolved lib $0 to $1", lib_name,
                                    container_libs[lib_idx].string());
        break;
      }
    }
  }
  return container_libs;
}

// Return error if something unexpected occurs.
// Return 0 if nothing unexpected, but there is nothing to deploy (e.g. no OpenSSL detected).
StatusOr<int> UProbeManager::AttachOpenSSLUProbes(
    uint32_t pid, ebpf::BPFHashTable<uint32_t, struct openssl_symaddrs_t>* openssl_symaddrs_map) {
  constexpr std::string_view kLibSSL = "libssl.so.1.1";
  constexpr std::string_view kLibCrypto = "libcrypto.so.1.1";
  const std::vector<std::string_view> lib_names = {kLibSSL, kLibCrypto};

  const system::Config& sysconfig = system::Config::GetInstance();

  // Find paths to libssl.so and libcrypto.so for the pid, if they are in use (i.e. mapped).
  PL_ASSIGN_OR_RETURN(const std::vector<std::filesystem::path> container_lib_paths,
                      FindLibraryPaths(lib_names, pid, proc_parser_.get()));

  std::filesystem::path container_libssl = container_lib_paths[0];
  std::filesystem::path container_libcrypto = container_lib_paths[1];

  if (container_libssl.empty() || container_libcrypto.empty()) {
    // Looks like this process doesn't use OpenSSL, because it did not
    // map both of libssl.so.x.x & libcrypto.so.x.x.
    // Return "0" to indicate zero probes were attached. This is not an error.
    return 0;
  }

  // Convert to host path, in case we're running inside a container ourselves.
  container_libssl = sysconfig.ToHostPath(container_libssl);
  container_libcrypto = sysconfig.ToHostPath(container_libcrypto);
  PL_RETURN_IF_ERROR(fs::Exists(container_libssl));
  PL_RETURN_IF_ERROR(fs::Exists(container_libcrypto));

  PL_RETURN_IF_ERROR(UpdateOpenSSLSymAddrs(container_libcrypto, pid, openssl_symaddrs_map));

  // Only try probing .so files that we haven't already set probes on.
  auto result = openssl_probed_binaries_.insert(container_libssl);
  if (!result.second) {
    return 0;
  }

  for (auto spec : kOpenSSLUProbes) {
    spec.binary_path = container_libssl.string();
    PL_RETURN_IF_ERROR(bcc_->AttachUProbe(spec));
  }
  return kOpenSSLUProbes.size();
}

StatusOr<int> UProbeManager::AttachGoTLSUProbes(
    const std::string& binary, obj_tools::ElfReader* elf_reader,
    obj_tools::DwarfReader* dwarf_reader, const std::vector<int32_t>& pids,
    ebpf::BPFHashTable<uint32_t, struct go_tls_symaddrs_t>* go_tls_symaddrs_map) {
  // Step 1: Update BPF symbols_map on all new PIDs.
  Status s = UpdateGoTLSSymAddrs(elf_reader, dwarf_reader, pids, go_tls_symaddrs_map);
  if (!s.ok()) {
    // Doesn't appear to be a binary with the mandatory symbols.
    // Might not even be a golang binary.
    // Either way, not of interest to probe.
    return 0;
  }

  // Step 2: Deploy uprobes on all new binaries.
  auto result = go_tls_probed_binaries_.insert(binary);
  if (!result.second) {
    // This is not a new binary, so nothing more to do.
    return 0;
  }
  return AttachUProbeTmpl(kGoTLSUProbeTmpls, binary, elf_reader);
}

namespace {

// Convert PID list from list of UPIDs to a map with key=binary name, value=PIDs
std::map<std::string, std::vector<int32_t>> ConvertPIDsListToMap(
    const absl::flat_hash_set<md::UPID>& upids) {
  const system::Config& sysconfig = system::Config::GetInstance();

  // Convert to a map of binaries, with the upids that are instances of that binary.
  std::map<std::string, std::vector<int32_t>> pids;

  PL_ASSIGN_OR(std::unique_ptr<FilePathResolver> fp_resolver, FilePathResolver::Create(),
               return {});

  // Consider new UPIDs only.
  for (const auto& upid : upids) {
    PL_ASSIGN_OR(std::filesystem::path proc_exe, ProcExe(upid.pid()), continue);

    Status s = fp_resolver->SetMountNamespace(upid.pid());
    if (!s.ok()) {
      VLOG(1) << absl::Substitute("Could not set pid namespace. Did the pid terminate?");
      continue;
    }

    PL_ASSIGN_OR(std::filesystem::path exe_path, fp_resolver->ResolvePath(proc_exe), continue);

    std::filesystem::path host_exe_path = sysconfig.ToHostPath(exe_path);
    if (!fs::Exists(host_exe_path).ok()) {
      continue;
    }
    pids[host_exe_path.string()].push_back(upid.pid());
  }

  LOG_FIRST_N(INFO, 1) << absl::Substitute("New PIDs count = $0", pids.size());

  return pids;
}

}  // namespace

std::thread UProbeManager::RunDeployUProbesThread(const absl::flat_hash_set<md::UPID>& pids) {
  // Increment before starting thread to avoid race in case thread starts late.
  ++num_deploy_uprobes_threads_;
  return std::thread([this, pids]() {
    DeployUProbes(pids);
    --num_deploy_uprobes_threads_;
  });
  return {};
}

void UProbeManager::CleanupSymaddrMaps(const absl::flat_hash_set<md::UPID>& deleted_upids) {
  for (const auto& pid : deleted_upids) {
    // TODO(oazizi): Enable these once we have a filter. Otherwise, these are expensive.
    // openssl_symaddrs_map_->remove_value(pid.pid());
    // go_common_symaddrs_map_->remove_value(pid.pid());
    // go_tls_symaddrs_map_->remove_value(pid.pid());
    go_http2_symaddrs_map_->remove_value(pid.pid());
  }
}

int UProbeManager::DeployOpenSSLUProbes(const absl::flat_hash_set<md::UPID>& pids) {
  int uprobe_count = 0;

  for (const auto& pid : pids) {
    if (cfg_disable_self_probing_ && pid.pid() == static_cast<uint32_t>(getpid())) {
      continue;
    }

    StatusOr<int> attach_status = AttachOpenSSLUProbes(pid.pid(), openssl_symaddrs_map_.get());
    if (!attach_status.ok()) {
      LOG_FIRST_N(WARNING, 10) << absl::Substitute("AttachOpenSSLUprobes failed for PID $0: $1",
                                                   pid.pid(), attach_status.ToString());
    } else {
      uprobe_count += attach_status.ValueOrDie();
    }
  }

  return uprobe_count;
}

int UProbeManager::DeployGoUProbes(const absl::flat_hash_set<md::UPID>& pids) {
  int uprobe_count = 0;

  for (const auto& [binary, pid_vec] : ConvertPIDsListToMap(pids)) {
    if (cfg_disable_self_probing_) {
      // Don't try to attach uprobes to self.
      // This speeds up stirling_wrapper initialization significantly.
      if (pid_vec.size() == 1 && pid_vec[0] == getpid()) {
        continue;
      }
    }

    // Read binary's symbols.
    StatusOr<std::unique_ptr<ElfReader>> elf_reader_status = ElfReader::Create(binary);
    if (!elf_reader_status.ok()) {
      LOG(WARNING) << absl::Substitute(
          "Cannot analyze binary $0 for uprobe deployment. "
          "If file is under /var/lib, container may have terminated. "
          "Message = $1",
          binary, elf_reader_status.msg());
      continue;
    }
    std::unique_ptr<ElfReader> elf_reader = elf_reader_status.ConsumeValueOrDie();

    // Avoid going passed this point if not a golang program.
    // The DwarfReader is memory intensive, and the remaining probes are Golang specific.
    // TODO(oazizi): Consolidate with similar check in dynamic_tracing/autogen.cc.
    bool is_golang_binary = elf_reader->SymbolAddress("runtime.buildVersion").has_value();
    if (!is_golang_binary) {
      continue;
    }

    StatusOr<std::unique_ptr<DwarfReader>> dwarf_reader_status = DwarfReader::Create(binary);
    if (!dwarf_reader_status.ok()) {
      VLOG(1) << absl::Substitute(
          "Failed to get binary $0 debug symbols. Cannot deploy uprobes. "
          "Message = $1",
          binary, dwarf_reader_status.msg());
      continue;
    }
    std::unique_ptr<DwarfReader> dwarf_reader = dwarf_reader_status.ConsumeValueOrDie();

    Status s = UpdateGoCommonSymAddrs(elf_reader.get(), dwarf_reader.get(), pid_vec,
                                      go_common_symaddrs_map_.get());
    if (!s.ok()) {
      LOG(WARNING) << absl::Substitute(
          "Golang binary $0 does not have the mandatory symbols (e.g. TCPConn).", binary);
      continue;
    }

    // GoTLS Probes.
    {
      StatusOr<int> attach_status = AttachGoTLSUProbes(binary, elf_reader.get(), dwarf_reader.get(),
                                                       pid_vec, go_tls_symaddrs_map_.get());
      if (!attach_status.ok()) {
        LOG_FIRST_N(WARNING, 10) << absl::Substitute("Failed to attach GoTLS Uprobes to $0: $1",
                                                     binary, attach_status.ToString());
      } else {
        uprobe_count += attach_status.ValueOrDie();
      }
    }

    // Go HTTP2 Probes.
    if (cfg_enable_http2_tracing_) {
      StatusOr<int> attach_status = AttachGoHTTP2Probes(
          binary, elf_reader.get(), dwarf_reader.get(), pid_vec, go_http2_symaddrs_map_.get());
      if (!attach_status.ok()) {
        LOG_FIRST_N(WARNING, 10) << absl::Substitute("Failed to attach HTTP2 Uprobes to $0: $1",
                                                     binary, attach_status.ToString());
      } else {
        uprobe_count += attach_status.ValueOrDie();
      }
    }
  }

  return uprobe_count;
}

absl::flat_hash_set<md::UPID> UProbeManager::PIDsToRescanForUProbes() {
  // Copy the snapshotted pids.
  // We later remove the corresponding pids from mmap_events_. We cannot simply clear mmap_events_
  // as it may be concurrently accessed from kernel-space.
  std::vector<std::pair<upid_t, bool>> mmap_event_entries = mmap_events_->get_table_offline();

  // Get the ASID, using an entry from proc_tracker.
  if (proc_tracker_.upids().empty()) {
    return {};
  }
  uint32_t asid = proc_tracker_.upids().begin()->asid();

  absl::flat_hash_set<md::UPID> upids_to_rescan;
  for (const auto& event : mmap_event_entries) {
    const upid_t& pid = event.first;
    md::UPID upid(asid, pid.pid, pid.start_time_ticks);

    if (proc_tracker_.upids().contains(upid) && !proc_tracker_.new_upids().contains(upid)) {
      upids_to_rescan.insert(upid);
    }

    mmap_events_->remove_value(pid);
  }

  return upids_to_rescan;
}

void UProbeManager::DeployUProbes(const absl::flat_hash_set<md::UPID>& pids) {
  const std::lock_guard<std::mutex> lock(deploy_uprobes_mutex_);

  proc_tracker_.Update(pids);

  // Before deploying new probes, clean-up map entries for old processes that are now dead.
  CleanupSymaddrMaps(proc_tracker_.deleted_upids());

  int uprobe_count = 0;

  uprobe_count += DeployOpenSSLUProbes(proc_tracker_.new_upids());
  if (FLAGS_stirling_rescan_for_dlopen) {
    uprobe_count += DeployOpenSSLUProbes(PIDsToRescanForUProbes());
  }
  uprobe_count += DeployGoUProbes(proc_tracker_.new_upids());

  LOG_FIRST_N(INFO, 1) << absl::Substitute("Number of uprobes deployed = $0", uprobe_count);
}

}  // namespace stirling
}  // namespace pl