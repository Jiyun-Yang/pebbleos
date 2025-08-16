/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flash_logging.h"

#include "console/dbgserial.h"
#include "drivers/flash.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "util/shared_circular_buffer.h"

#include "services/common/system_task.h"

static SharedCircularBuffer s_buffer;
static SharedCircularBufferClient s_buffer_client;
// During normal operation, since log messages are hashed, most are only 30-40
// bytes long with the longest being about 80 bytes, so this is enough for 15-40
// or so messages.
static uint8_t s_buffer_storage[1200];
static PebbleMutex *s_buffer_mutex = INVALID_MUTEX_HANDLE; //!< Protects s_buffer
static PebbleMutex *s_flash_write_mutex = INVALID_MUTEX_HANDLE; //!< Protects log line consistency
static bool s_is_flash_write_scheduled; //!< true if handle_buffer_sync KernelBG callback is scheduled

static void write_message(void) {
  // Note that we should enter this function with the buffer mutex held.

  const uint8_t *data_read;
  uint16_t read_length;

  // Read the header part
  bool result = shared_circular_buffer_read(&s_buffer, &s_buffer_client, sizeof(uint8_t), &data_read, &read_length);
  PBL_ASSERTN(result);
  PBL_ASSERT(read_length == sizeof(uint8_t), "read_length %u sizeof(uint8_t) %u", read_length, sizeof(uint8_t));
  uint8_t msg_length = *data_read;

  if (shared_circular_buffer_get_read_space_remaining(&s_buffer, &s_buffer_client) < msg_length + sizeof(uint8_t)) {
    return; // Not ready yet, consume nothing.
  }

  // Flash_logging_log_start can trigger a flash erase. Release the buffer mutex
  // to allow logging while the (slow) erase completes.
  mutex_unlock(s_buffer_mutex);
  uint32_t flash_addr = flash_logging_log_start(msg_length);
  mutex_lock(s_buffer_mutex);
  if (flash_addr == FLASH_LOG_INVALID_ADDR) {
    return;
  }

  shared_circular_buffer_consume(&s_buffer, &s_buffer_client, read_length);

  while (msg_length > 0) {
    mutex_unlock(s_buffer_mutex);

    // Note that this buffer read really should be done with the buffer mutex held.
    // This works only because writes to the buffer do not advance slackers.
    result = shared_circular_buffer_read(&s_buffer, &s_buffer_client, msg_length, &data_read, &read_length);
    PBL_ASSERTN(result);
    msg_length -= read_length;

    flash_logging_write(data_read, flash_addr, read_length);
    flash_addr += read_length;

    mutex_lock(s_buffer_mutex);
    shared_circular_buffer_consume(&s_buffer, &s_buffer_client, read_length);
  }

  // We should still be holding the buffer lock here...
}

static void handle_buffer_sync(void *data) {
  const bool is_async = (uintptr_t) data;

  mutex_lock(s_flash_write_mutex);
  mutex_lock(s_buffer_mutex);

  while (shared_circular_buffer_get_read_space_remaining(&s_buffer, &s_buffer_client) > 0) {
    write_message();
    // The above function mucks with the mutex
    mutex_assert_held_by_curr_task(s_buffer_mutex, true /* is_held */);
  }

  if (is_async) {
    s_is_flash_write_scheduled = false;
  }

  mutex_unlock(s_buffer_mutex);
  mutex_unlock(s_flash_write_mutex);
}


void advanced_logging_init(void) {
  flash_logging_init();

  shared_circular_buffer_init(&s_buffer, s_buffer_storage, sizeof(s_buffer_storage));
  shared_circular_buffer_add_client(&s_buffer, &s_buffer_client);

  s_buffer_mutex = mutex_create();
  s_flash_write_mutex = mutex_create();
}

// Return true on success
static bool write_buffer_locking(char* buffer, int length, bool async) {
  bool success = false;

  do {
    mutex_lock(s_buffer_mutex);
    if (shared_circular_buffer_get_write_space_remaining(&s_buffer) >= length + 1) {
      // Ideally we could figure out a way to skip out on this copy but then you'd potentially need to sniprintf
      // into a non-contiguous buffer... whatever, we have CPU to burn.
      uint8_t msg_length = length;

      // Do not advance slackers. Data loss and/or corruption will occur! See write_message()
      shared_circular_buffer_write(&s_buffer, &msg_length, sizeof(uint8_t), false /*advance_slackers*/);
      shared_circular_buffer_write(&s_buffer, (const uint8_t*) buffer, length, false /*advance_slackers*/);

      success = true;
    }
    mutex_unlock(s_buffer_mutex);

    // If we failed to buffer this message, flush the buffer to cache to make room.
    // Otherwise, if this is a sync message, flush this message to flash.
    if (!success || !async) {
      handle_buffer_sync((void *)(uintptr_t) false /* !is_async */);
    }
  } while (!success); // Loop until the buffer copy succeeds. If sync, also wait until this message
                      // is written to flash.
                      // It's highly unlikely that another task will win the race and completely
                      // fill the buffer between the flash write and the next buffer write attempt.
                      // If so, there are bigger issues.

  if (async) {
    mutex_lock(s_buffer_mutex);
    if (!s_is_flash_write_scheduled) {
      s_is_flash_write_scheduled = true;
      system_task_add_callback(handle_buffer_sync, (void *)(uintptr_t) true /* is_async */);
    }
    mutex_unlock(s_buffer_mutex);
  }

  return success;
}

void pbl_log_advanced(char* buffer, int length, bool async) {
  if (s_buffer_mutex == INVALID_MUTEX_HANDLE) {
    return;
  }
  write_buffer_locking(buffer, length, async);
}

char pbl_log_get_level_char(const uint8_t log_level) {
  switch (log_level) {
  case LOG_LEVEL_ALWAYS:
    return '*';
  case LOG_LEVEL_ERROR:
    return 'E';
  case LOG_LEVEL_WARNING:
    return 'W';
  case LOG_LEVEL_INFO:
    return 'I';
  case LOG_LEVEL_DEBUG:
    return 'D';
  case LOG_LEVEL_DEBUG_VERBOSE:
    return 'V';
  default:
    return '?';
  }
}

