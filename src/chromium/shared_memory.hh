// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUM_SHARED_MEMORY_HH_
#define CHROMIUM_SHARED_MEMORY_HH_
#pragma once

#include <cstdint>
#include <string>

/* Boost noncopyable base class */
#include <boost/utility.hpp>

#include <windows.h>

class FilePath;

namespace chromium {

// SharedMemoryHandle is a platform specific type which represents
// the underlying OS handle to a shared memory segment.
typedef HANDLE SharedMemoryHandle;
typedef HANDLE SharedMemoryLock;

// Options for creating a shared memory object.
struct SharedMemoryCreateOptions {
  SharedMemoryCreateOptions() : name(nullptr), size(0), open_existing(false),
                                executable(false) {}

  // If NULL, the object is anonymous.  This pointer is owned by the caller
  // and must live through the call to Create().
  const std::string* name;

  // Size of the shared memory object to be created.
  // When opening an existing object, this has no effect.
  uint32_t size;

  // If true, and the shared memory already exists, Create() will open the
  // existing shared memory and ignore the size parameter.  If false,
  // shared memory must not exist.  This flag is meaningless unless name is
  // non-NULL.
  bool open_existing;

  // If true, mappings might need to be made executable later.
  bool executable;
};

// Platform abstraction for shared memory.  Provides a C++ wrapper
// around the OS primitive for a memory mapped file.
class SharedMemory : boost::noncopyable {
 public:
  SharedMemory();

  // Similar to the default constructor, except that this allows for
  // calling Lock() to acquire the named mutex before either Create or Open
  // are called on Windows.
  explicit SharedMemory(const std::string& name);

  // Create a new SharedMemory object from an existing, open
  // shared memory file.
  SharedMemory(SharedMemoryHandle handle, bool read_only);

  // Closes any open files.
  ~SharedMemory();

  // Return true iff the given handle is valid (i.e. not the distingished
  // invalid value; NULL for a HANDLE and -1 for a file descriptor)
  static bool IsHandleValid(const SharedMemoryHandle& handle);

  // Returns invalid handle (see comment above for exact definition).
  static SharedMemoryHandle NULLHandle();

  // Closes a shared memory handle.
  static void CloseHandle(const SharedMemoryHandle& handle);

  // Creates a shared memory object as described by the options struct.
  // Returns true on success and false on failure.
  bool Create(const SharedMemoryCreateOptions& options);

  // Creates and maps an anonymous shared memory segment of size size.
  // Returns true on success and false on failure.
  bool CreateAndMapAnonymous(uint32_t size);

  // Creates an anonymous shared memory segment of size size.
  // Returns true on success and false on failure.
  bool CreateAnonymous(uint32_t size) {
    SharedMemoryCreateOptions options;
    options.size = size;
    return Create(options);
  }

  // Creates or opens a shared memory segment based on a name.
  // If open_existing is true, and the shared memory already exists,
  // opens the existing shared memory and ignores the size parameter.
  // If open_existing is false, shared memory must not exist.
  // size is the size of the block to be created.
  // Returns true on success, false on failure.
  bool CreateNamed(const std::string& name, bool open_existing, uint32_t size) {
    SharedMemoryCreateOptions options;
    options.name = &name;
    options.open_existing = open_existing;
    options.size = size;
    return Create(options);
  }

  // Deletes resources associated with a shared memory segment based on name.
  // Not all platforms require this call.
  bool Delete(const std::string& name);

  // Opens a shared memory segment based on a name.
  // If read_only is true, opens for read-only access.
  // Returns true on success, false on failure.
  bool Open(const std::string& name, bool read_only);

  // Maps the shared memory into the caller's address space.
  // Returns true on success, false otherwise.  The memory address
  // is accessed via the memory() accessor.  The mapped address is guaranteed to
  // have an alignment of at least MAP_MINIMUM_ALIGNMENT.
  bool Map(uint32_t bytes);
  enum { MAP_MINIMUM_ALIGNMENT = 32 };

  // Unmaps the shared memory from the caller's address space.
  // Returns true if successful; returns false on error or if the
  // memory is not mapped.
  bool Unmap();

  // Get the size of the shared memory backing file.
  // Note:  This size is only available to the creator of the
  // shared memory, and not to those that opened shared memory
  // created externally.
  // Returns 0 if not created or unknown.
  // Deprecated method, please keep track of the size yourself if you created
  // it.
  // http://crbug.com/60821
  uint32_t created_size() const { return created_size_; }

  // Gets a pointer to the opened memory space if it has been
  // Mapped via Map().  Returns NULL if it is not mapped.
  void *memory() const { return memory_; }

  // Returns the underlying OS handle for this segment.
  // Use of this handle for anything other than an opaque
  // identifier is not portable.
  SharedMemoryHandle handle() const;

  // Closes the open shared memory segment.
  // It is safe to call Close repeatedly.
  void Close();

  // Locks the shared memory.
  //
  // WARNING: on POSIX the memory locking primitive only works across
  // processes, not across threads.  The Lock method is not currently
  // used in inner loops, so we protect against multiple threads in a
  // critical section using a class global lock.
  void Lock();

#if defined(_WIN32)
  // A Lock() implementation with a timeout that also allows setting
  // security attributes on the mutex. sec_attr may be NULL.
  // Returns true if the Lock() has been acquired, false if the timeout was
  // reached.
  bool Lock(uint32_t timeout_ms, SECURITY_ATTRIBUTES* sec_attr);
#endif

  // Releases the shared memory lock.
  void Unlock();

 private:
#if defined(_WIN32)
  std::string        name_;
  HANDLE             mapped_file_;
#endif
  void*              memory_;
  bool               read_only_;
  uint32_t           created_size_;
  SharedMemoryLock   lock_;
};

// A helper class that acquires the shared memory lock while
// the SharedMemoryAutoLock is in scope.
class SharedMemoryAutoLock : boost::noncopyable {
 public:
  explicit SharedMemoryAutoLock(SharedMemory* shared_memory)
      : shared_memory_(shared_memory) {
    shared_memory_->Lock();
  }

  ~SharedMemoryAutoLock() {
    shared_memory_->Unlock();
  }

 private:
  SharedMemory* shared_memory_;
};

}  // namespace chromium

#endif  // CHROMIUM_SHARED_MEMORY_HH_