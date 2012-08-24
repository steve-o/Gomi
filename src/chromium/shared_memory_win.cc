// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shared_memory.hh"

#include "logging.hh"

namespace chromium {

SharedMemory::SharedMemory()
    : mapped_file_(nullptr),
      memory_(nullptr),
      read_only_(false),
      created_size_(0),
      lock_(nullptr) {
}

SharedMemory::SharedMemory(const std::string& name)
    : mapped_file_(nullptr),
      memory_(NULL),
      read_only_(false),
      created_size_(0),
      lock_(nullptr),
      name_(name) {
}

SharedMemory::SharedMemory(SharedMemoryHandle handle, bool read_only)
    : mapped_file_(handle),
      memory_(nullptr),
      read_only_(read_only),
      created_size_(0),
      lock_(nullptr) {
}

SharedMemory::~SharedMemory() {
  Close();
  if (lock_ != nullptr)
    CloseHandle(lock_);
}

// static
bool SharedMemory::IsHandleValid(const SharedMemoryHandle& handle) {
  return handle != nullptr;
}

// static
SharedMemoryHandle SharedMemory::NULLHandle() {
  return nullptr;
}

// static
void SharedMemory::CloseHandle(const SharedMemoryHandle& handle) {
  DCHECK(handle != nullptr);
  ::CloseHandle(handle);
}

bool SharedMemory::CreateAndMapAnonymous(uint32_t size) {
  return CreateAnonymous(size) && Map(size);
}

bool SharedMemory::Create(const SharedMemoryCreateOptions& options) {
  DCHECK(!options.executable);
  DCHECK(!mapped_file_);
  if (options.size == 0)
    return false;

  // NaCl's memory allocator requires 0mod64K alignment and size for
  // shared memory objects.  To allow passing shared memory to NaCl,
  // therefore we round the size actually created to the nearest 64K unit.
  // To avoid client impact, we continue to retain the size as the
  // actual requested size.
  uint32_t rounded_size = (options.size + 0xffff) & ~0xffff;
  name_ = options.name == NULL ? "" : *options.name;
  mapped_file_ = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
      PAGE_READWRITE, 0, static_cast<DWORD>(rounded_size),
      name_.empty() ? NULL : name_.c_str());
  if (!mapped_file_)
    return false;

  created_size_ = options.size;

  // Check if the shared memory pre-exists.
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // If the file already existed, set created_size_ to 0 to show that
    // we don't know the size.
    created_size_ = 0;
    if (!options.open_existing) {
      Close();
      return false;
    }
  }

  return true;
}

bool SharedMemory::Delete(const std::string& name) {
  // intentionally empty -- there is nothing for us to do on Windows.
  return true;
}

bool SharedMemory::Open(const std::string& name, bool read_only) {
  DCHECK(!mapped_file_);

  name_ = name;
  read_only_ = read_only;
  mapped_file_ = OpenFileMappingA(
      read_only_ ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, false,
      name_.empty() ? nullptr : name_.c_str());
  if (mapped_file_ != nullptr) {
    // Note: size_ is not set in this case.
    return true;
  }
  return false;
}

bool SharedMemory::Map(uint32_t bytes) {
  if (mapped_file_ == NULL)
    return false;

  memory_ = MapViewOfFile(mapped_file_,
      read_only_ ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (memory_ != nullptr) {
    DCHECK_EQ(0U, reinterpret_cast<uintptr_t>(memory_) &
        (SharedMemory::MAP_MINIMUM_ALIGNMENT - 1));
    return true;
  }
  return false;
}

bool SharedMemory::Unmap() {
  if (memory_ == nullptr)
    return false;

  UnmapViewOfFile(memory_);
  memory_ = nullptr;
  return true;
}

void SharedMemory::Close() {
  if (memory_ != nullptr) {
    UnmapViewOfFile(memory_);
    memory_ = nullptr;
  }

  if (mapped_file_ != nullptr) {
    CloseHandle(mapped_file_);
    mapped_file_ = nullptr;
  }
}

void SharedMemory::Lock() {
  Lock(INFINITE, nullptr);
}

bool SharedMemory::Lock(uint32_t timeout_ms, SECURITY_ATTRIBUTES* sec_attr) {
  if (lock_ == nullptr) {
    std::string name = name_;
    name.append("lock");
    lock_ = CreateMutexA(sec_attr, FALSE, name.c_str());
    if (lock_ == nullptr) {
      DLOG(ERROR) << "Could not create mutex.";
      return false;  // there is nothing good we can do here.
    }
  }
  DWORD result = WaitForSingleObject(lock_, timeout_ms);

  // Return false for WAIT_ABANDONED, WAIT_TIMEOUT or WAIT_FAILED.
  return (result == WAIT_OBJECT_0);
}

void SharedMemory::Unlock() {
  DCHECK(lock_ != nullptr);
  ReleaseMutex(lock_);
}

SharedMemoryHandle SharedMemory::handle() const {
  return mapped_file_;
}

}  // namespace chromium

/* eof */