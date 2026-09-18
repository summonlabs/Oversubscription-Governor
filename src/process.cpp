#include "oversub/process.hpp"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace oversub {
namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::wstring quote(const std::string& text) {
  std::wstring wide = widen(text);
  std::wstring out = L"\"";
  for (const wchar_t ch : wide) {
    if (ch == L'"') out += L'\\';
    out += ch;
  }
  out += L"\"";
  return out;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() { reset(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
#if defined(_WIN32)
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  os_pid_ = other.os_pid_;
  other.os_pid_ = -1;
#endif
  pid_ = other.pid_;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    reset();
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    os_pid_ = other.os_pid_;
    other.os_pid_ = -1;
#endif
    pid_ = other.pid_;
    other.pid_ = 0;
  }
  return *this;
}

void ChildProcess::reset() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  os_pid_ = -1;
#endif
  pid_ = 0;
}

bool ChildProcess::valid() const {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return os_pid_ > 0;
#endif
}

Status ChildProcess::spawn(const ProcessSpec& spec) {
  if (spec.executable.empty()) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::InvalidField, "executable path is empty");
  }
  if (valid()) {
    return Status::failure(StatusCode::AlreadyExists, ReasonCode::None, "process handle is already in use");
  }
#if defined(_WIN32)
  std::wstring command = quote(spec.executable);
  for (const auto& argument : spec.arguments) {
    command += L" ";
    command += quote(argument);
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const std::wstring working = spec.working_directory.empty() ? std::wstring() : widen(spec.working_directory);
  const DWORD flags = spec.inherit_output ? 0 : CREATE_NO_WINDOW;
  if (!::CreateProcessW(widen(spec.executable).c_str(), mutable_command.data(), nullptr, nullptr,
                        spec.inherit_output ? TRUE : FALSE, flags,
                        nullptr, working.empty() ? nullptr : working.c_str(), &startup, &info)) {
    return Status::failure(StatusCode::IoError, ReasonCode::None,
                           "CreateProcess failed with error " + std::to_string(::GetLastError()));
  }
  ::CloseHandle(info.hThread);
  handle_ = info.hProcess;
  pid_ = static_cast<u32>(info.dwProcessId);
  return Status::success();
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "fork failed");
  }
  if (pid == 0) {
    if (!spec.working_directory.empty()) {
      if (::chdir(spec.working_directory.c_str()) != 0) ::_exit(127);
    }
    std::vector<char*> argv;
    argv.reserve(spec.arguments.size() + 2);
    argv.push_back(const_cast<char*>(spec.executable.c_str()));
    for (const auto& argument : spec.arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(spec.executable.c_str(), argv.data());
    ::_exit(127);
  }
  os_pid_ = pid;
  pid_ = static_cast<u32>(pid);
  return Status::success();
#endif
}

bool ChildProcess::running() {
  if (!valid()) return false;
#if defined(_WIN32)
  const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(handle_), 0);
  return result == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(os_pid_), &status, WNOHANG);
  if (result == 0) return true;
  return false;
#endif
}

Status ChildProcess::wait(u32& exit_code) {
  if (!valid()) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::None, "process handle is not valid");
  }
#if defined(_WIN32)
  const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  if (result != WAIT_OBJECT_0) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "WaitForSingleObject failed");
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code)) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "GetExitCodeProcess failed");
  }
  exit_code = static_cast<u32>(code);
  reset();
  return Status::success();
#else
  int status = 0;
  if (::waitpid(static_cast<pid_t>(os_pid_), &status, 0) < 0) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "waitpid failed");
  }
  exit_code = WIFEXITED(status) ? static_cast<u32>(WEXITSTATUS(status)) : 1u;
  reset();
  return Status::success();
#endif
}

Status ChildProcess::terminate() {
  if (!valid()) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::None, "process handle is not valid");
  }
#if defined(_WIN32)
  if (!::TerminateProcess(static_cast<HANDLE>(handle_), 1)) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "TerminateProcess failed");
  }
  return Status::success();
#else
  if (::kill(static_cast<pid_t>(os_pid_), SIGKILL) != 0) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "kill failed");
  }
  return Status::success();
#endif
}

Status current_executable_path(std::string& out) {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return Status::failure(StatusCode::IoError, ReasonCode::None, "GetModuleFileName failed");
    }
    if (length < buffer.size()) {
      buffer.resize(length + 1);
      buffer[length] = L'\0';
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  const std::filesystem::path path(buffer.data());
  out = path.string();
  return Status::success();
#else
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return Status::failure(StatusCode::IoError, ReasonCode::None, "cannot resolve /proc/self/exe");
  }
  out = path.string();
  return Status::success();
#endif
}

Status current_executable_directory(std::string& out) {
  std::string path;
  Status status = current_executable_path(path);
  if (!status.ok()) return status;
  out = std::filesystem::path(path).parent_path().string();
  return Status::success();
}

}  // namespace oversub
