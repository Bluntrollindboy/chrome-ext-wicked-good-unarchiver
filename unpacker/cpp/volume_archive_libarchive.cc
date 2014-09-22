// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "volume_archive_libarchive.h"

#include <algorithm>
#include <cerrno>

#include "archive_entry.h"
#include "ppapi/cpp/logging.h"

namespace {

const int64_t kArchiveReadDataError = -1;  // Negative value means error.

std::string ArchiveError(const std::string& message, archive* archive_object) {
  return message + archive_error_string(archive_object);
}

// Sets the libarchive internal error to a VolumeReader related error.
// archive_error_string function must work on valid strings, but in case of
// errors in the custom functions, libarchive API assumes the error is set by
// us. If we don't set it, we will get a Segmentation Fault because
// archive_error_string will work on invalid memory.
void SetLibarchiveErrorToVolumeReaderError(archive* archive_object) {
  archive_set_error(archive_object,
                    EIO /* I/O error. */,
                    "%s" /* Format string similar to printf. */,
                    volume_archive_constants::kVolumeReaderError);
}

ssize_t CustomArchiveRead(archive* archive_object,
                          void* client_data,
                          const void** buffer) {
  VolumeArchiveLibarchive* volume_archive =
      static_cast<VolumeArchiveLibarchive*>(client_data);

  // Get header data from local cache.
  if (volume_archive->header_read()) {
    ssize_t header_size = 0;
    *buffer = volume_archive->header_cache()->GetHeader(
        volume_archive->reader()->GetOffset(), &header_size);

    if (*buffer) {
      // Advance reader to be in sync with VolumeArchive.
      if (volume_archive->reader()->Skip(header_size) != header_size) {
        SetLibarchiveErrorToVolumeReaderError(archive_object);
        return ARCHIVE_FATAL;
      }

      return header_size;
    }
  }

  // Get offset for HeaderCache before calling VolumeReader::Read. After read
  // operation, the VolumeReader offset will be changed to the current offset
  // plus the number of read bytes, but HeaderCache should store the header data
  // starting from the current offset, not the offset after VolumeReader::Read.
  int64_t offset = volume_archive->reader()->GetOffset();

  // Read data.
  ssize_t read_bytes = volume_archive->reader()->Read(
      volume_archive->reader_data_size(), buffer);

  if (read_bytes == ARCHIVE_FATAL) {
    SetLibarchiveErrorToVolumeReaderError(archive_object);
    return ARCHIVE_FATAL;
  }

  // Save header data in HeaderCache.
  if (volume_archive->header_read() && read_bytes > 0) {
    volume_archive->header_cache()->AddHeader(offset,
                                              static_cast<const char*>(*buffer),
                                              read_bytes);
  }

  return read_bytes;
}

int64_t CustomArchiveSkip(archive* archive_object,
                          void* client_data,
                          int64_t request) {
  VolumeArchiveLibarchive* volume_archive =
      static_cast<VolumeArchiveLibarchive*>(client_data);
  // VolumeReader::Skip returns 0 in case of failure and CustomArchiveRead is
  // used instead, so there is no need to check for VolumeReader error.
  return volume_archive->reader()->Skip(request);
}

int64_t CustomArchiveSeek(archive* archive_object,
                          void* client_data,
                          int64_t offset,
                          int whence) {
  VolumeArchiveLibarchive* volume_archive =
      static_cast<VolumeArchiveLibarchive*>(client_data);

  int64_t new_offset = volume_archive->reader()->Seek(offset, whence);
  if (new_offset == ARCHIVE_FATAL)
    SetLibarchiveErrorToVolumeReaderError(archive_object);

  return new_offset;
}

int CustomArchiveClose(archive* archive_object, void* client_data) {
  VolumeArchiveLibarchive* volume_archive =
      static_cast<VolumeArchiveLibarchive*>(client_data);

  int result = volume_archive->reader()->Close();
  if (result == ARCHIVE_FATAL)
    SetLibarchiveErrorToVolumeReaderError(archive_object);
  return result;
}

}  // namespace

VolumeArchiveLibarchive::VolumeArchiveLibarchive(const std::string& request_id,
                                                 VolumeReader* reader,
                                                 HeaderCache* header_cache)
    : VolumeArchive(request_id, reader),
      header_cache_(header_cache),
      header_read_(false),
      // Reader size is volume_archive_constants::kHeaderChunkSize
      // because at first archive headers are read.
      reader_data_size_(volume_archive_constants::kHeaderChunkSize),
      archive_(NULL),
      current_archive_entry_(NULL),
      last_read_data_offset_(0),
      last_read_data_length_(0),
      decompressed_data_(NULL),
      decompressed_data_size_(0),
      decompressed_error_(false) {
}

VolumeArchiveLibarchive::~VolumeArchiveLibarchive() {
  Cleanup();
}

bool VolumeArchiveLibarchive::Init() {
  archive_ = archive_read_new();
  if (!archive_) {
    set_error_message(volume_archive_constants::kArchiveReadNewError);
    return false;
  }

  // TODO(cmihail): Once the bug mentioned at
  // https://code.google.com/p/libarchive/issues/detail?id=262 is resolved
  // add RAR file handler to manifest.json.
  if (archive_read_support_format_rar(archive_) != ARCHIVE_OK ||
      archive_read_support_format_zip(archive_) != ARCHIVE_OK) {
    set_error_message(ArchiveError(
        volume_archive_constants::kArchiveSupportErrorPrefix, archive_));
    return false;
  }

  // Set callbacks for processing the archive's data and open the archive.
  // The callback data is the VolumeArchive itself.
  header_read_ = true;  // archive_read_open1 will read the archive header.
  int ok = ARCHIVE_OK;
  if (archive_read_set_read_callback(archive_, CustomArchiveRead) != ok ||
      archive_read_set_skip_callback(archive_, CustomArchiveSkip) != ok ||
      archive_read_set_seek_callback(archive_, CustomArchiveSeek) != ok ||
      archive_read_set_close_callback(archive_, CustomArchiveClose) != ok ||
      archive_read_set_callback_data(archive_, this) != ok ||
      archive_read_open1(archive_) != ok) {
    set_error_message(ArchiveError(
        volume_archive_constants::kArchiveOpenErrorPrefix, archive_));
    return false;
  }

  return true;
}

bool VolumeArchiveLibarchive::GetNextHeader(const char** pathname,
                                            int64_t* size,
                                            bool* is_directory,
                                            time_t* modification_time) {
  header_read_ = true;  // archive_read_next_header will read the file header.

  // Reset VolumeReader data size so CustomArchiveRead doesn't require big
  // chunks for headers.
  reader_data_size_ = volume_archive_constants::kHeaderChunkSize;

  // Reset to 0 for new VolumeArchive::ReadData operation.
  last_read_data_offset_ = 0;

  // Archive data is skipped automatically by next call to
  // archive_read_next_header.
  switch (archive_read_next_header(archive_, &current_archive_entry_)) {
    case ARCHIVE_EOF:
      *pathname = NULL;  // End of archive.
      return true;
    case ARCHIVE_OK:
      *pathname = archive_entry_pathname(current_archive_entry_);
      *size = archive_entry_size(current_archive_entry_);
      *modification_time = archive_entry_mtime(current_archive_entry_);
      *is_directory = S_ISDIR(archive_entry_filetype(current_archive_entry_));
      return true;
    default:
      set_error_message(ArchiveError(
          volume_archive_constants::kArchiveNextHeaderErrorPrefix, archive_));
      return false;
  }
}

void VolumeArchiveLibarchive::DecompressData(int64_t offset, int64_t length) {
  // TODO(cmihail): As an optimization consider using archive_read_data_block
  // which avoids extra copying in case offset != last_read_data_offset_.
  // The logic will be more complicated because archive_read_data_block offset
  // will not be aligned with the offset of the read request from JavaScript.
  header_read_ = false;  // Only headers should be cached, not data.

  // Request with offset smaller than last read offset.
  if (offset < last_read_data_offset_) {
    std::string file_path_name(archive_entry_pathname(current_archive_entry_));

    // Cleanup old archive. Don't delete VolumeReader as it will be reused.
    if (archive_read_free(archive_) != ARCHIVE_OK) {
      set_error_message(ArchiveError(
          volume_archive_constants::kArchiveReadDataErrorPrefix, archive_));
      decompressed_error_ = true;
      return;
    }
    reader()->Seek(0, SEEK_SET);  // Reset reader.

    // Reinitialize archive.
    if (!Init()) {
      decompressed_error_ = true;
      return;
    }

    // Reach file data by iterating through
    // VolumeArchiveLibarchive::GetNextHeader.
    const char* path_name = NULL;
    int64_t file_size = 0;
    bool is_directory = false;
    time_t modification_time = 0;
    for (;;) {
      if (!GetNextHeader(
              &path_name, &file_size, &is_directory, &modification_time)) {
        decompressed_error_ = true;
        return;
      }
      if (!path_name) {
        set_error_message(volume_archive_constants::kFileNotFound);
        decompressed_error_ = true;
        return;
      }

      if (file_path_name == std::string(path_name))
        break;  // File reached.
    }
    // Data offset was already reset to 0 by
    // VolumeArchiveLibarchive::GetNextHeader.
  }

  // Request with offset greater than last read offset. Skip not needed bytes.
  // Because files are compressed, seeking is not possible, so all of the bytes
  // until the requested position must be unpacked.
  ssize_t size = -1;
  while (offset > last_read_data_offset_) {
    // ReadData will call CustomArchiveRead when calling archive_read_data. Read
    // should not request more bytes than possibly needed, so we request either
    // offset - last_read_data_offset_, kMaximumDataChunkSize in case the former
    // is too big or kMinimumDataChunkSize in case its too small and we might
    // end up with too many IPCs.
    reader_data_size_ =
        std::max(std::min(offset - last_read_data_offset_,
                          volume_archive_constants::kMaximumDataChunkSize),
                 volume_archive_constants::kMinimumDataChunkSize);

    // No need for an offset in dummy_buffer as it will be ignored anyway.
    size =
        archive_read_data(archive_,
                          dummy_buffer_,
                          std::min(offset - last_read_data_offset_,
                                   volume_archive_constants::kDummyBufferSize));
    PP_DCHECK(size != 0);  // The actual read is done below. We shouldn't get to
                           // end of file here.
    if (size < 0) {        // Error.
      set_error_message(ArchiveError(
          volume_archive_constants::kArchiveReadDataErrorPrefix, archive_));
      decompressed_error_ = true;
      return;
    }
    last_read_data_offset_ += size;
  }

  // Do not decompress more bytes than we can store internally. The
  // kDecompressBufferSize limit is used to avoid huge memory usage.
  int64_t left_length =
      std::min(length, volume_archive_constants::kDecompressBufferSize);

  // ReadData will call CustomArchiveRead when calling archive_read_data. The
  // read should be done with a value similar to length, which is the requested
  // number of bytes, or kMaximumDataChunkSize / kMinimumDataChunkSize
  // in case length is too big or too small.
  reader_data_size_ =
      std::max(std::min(static_cast<int64_t>(left_length),
                        volume_archive_constants::kMaximumDataChunkSize),
               volume_archive_constants::kMinimumDataChunkSize);

  // Perform the actual copy.
  int64_t bytes_read = 0;
  do {
    size = archive_read_data(
        archive_, decompressed_data_buffer_ + bytes_read, left_length);
    if (size < 0) {  // Error.
      set_error_message(ArchiveError(
          volume_archive_constants::kArchiveReadDataErrorPrefix, archive_));
      decompressed_error_ = true;
      return;
    }
    bytes_read += size;
    left_length -= size;
  } while (left_length > 0 && size != 0);  // There is still data to read.

  // VolumeArchiveLibarchive::DecompressData always stores the data from
  // beginning of the buffer. VolumeArchiveLibarchive::ConsumeData is used
  // to preserve the bytes that are decompressed but not required by
  // VolumeArchiveLibarchive::ReadData.
  decompressed_data_ = decompressed_data_buffer_;
  decompressed_data_size_ = bytes_read;
}

bool VolumeArchiveLibarchive::Cleanup() {
  bool returnValue = true;
  if (archive_ && archive_read_free(archive_) != ARCHIVE_OK) {
    set_error_message(ArchiveError(
        volume_archive_constants::kArchiveReadFreeErrorPrefix, archive_));
    returnValue = false;  // Cleanup should release all resources even
                          // in case of failures.
  }
  archive_ = NULL;

  CleanupReader();

  return returnValue;
}

int64_t VolumeArchiveLibarchive::ReadData(int64_t offset,
                                          int64_t length,
                                          const char** buffer) {
  PP_DCHECK(length > 0);              // Length must be at least 1.
  PP_DCHECK(current_archive_entry_);  // Check that GetNextHeader was called at
                                      // least once. In case it wasn't, this is
                                      // a programmer error.

  // End of archive.
  if (archive_entry_size(current_archive_entry_) <= offset)
    return 0;

  // In case of first read or no more available data in the internal buffer or
  // offset is different from the last_read_data_offset_, then force
  // VolumeArchiveLibarchive::DecompressData as the decompressed data is
  // invalid.
  if (!decompressed_data_ || last_read_data_offset_ != offset ||
      decompressed_data_size_ == 0)
    DecompressData(offset, length);

  // Decompressed failed.
  if (decompressed_error_)
    return kArchiveReadDataError;

  last_read_data_length_ = length;  // Used for decompress ahead.

  // Assign the output *buffer parameter to the internal buffer.
  *buffer = decompressed_data_;

  // Advance internal buffer for next ReadData call.
  int64_t read_bytes = std::min(decompressed_data_size_, length);
  decompressed_data_ = decompressed_data_ + read_bytes;
  decompressed_data_size_ -= read_bytes;
  last_read_data_offset_ += read_bytes;

  PP_DCHECK(decompressed_data_ + decompressed_data_size_ <=
            decompressed_data_buffer_ +
                volume_archive_constants::kDecompressBufferSize);

  return read_bytes;
}

void VolumeArchiveLibarchive::MaybeDecompressAhead() {
  if (decompressed_data_size_ == 0)
    DecompressData(last_read_data_offset_, last_read_data_length_);
}