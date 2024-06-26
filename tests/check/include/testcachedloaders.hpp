#pragma once

#include <sstream>
#include <gst/gst.h>
#include "gsts3uploader.h"
#include "gsts3downloader.h"
#include "gsts3uploaderconfig.h"

#include <gsts3uploaderpartcache.hpp>

using gst::aws::s3::UploaderPartCache;

typedef struct {
    GstS3Uploader base;

    UploaderPartCache *cache;

    gint current_part_num;

    gint upload_part_count;
    gint upload_copy_part_count;
    gint cache_hits;
    gint cache_misses;
    gsize buffer_size;
} TestCachedUploader;

typedef struct {
  gint upload_part_count;
  gint upload_copy_part_count;
  gint cache_hits;
  gint cache_misses;
} TestCachedUploaderStats;

static TestCachedUploaderStats prev_test_cached_uploader_stats;

// cannot put this by value in our glib-created TestCachedUploader
// as that causes a segfault.
// 'uploader_buffer' -> The uploader's parts pushed to S3 already but not finalized via complete()
// 'uploaded_buffer' -> The result of an uploader complete()
std::string uploaded_buffer;
std::stringstream uploader_buffer;

#define PRINT_BUFFER(b, first, last) (for (int __i = first; __i < last; __i++) GST_TRACE("offset %10ld = %4d", __i, b[__i]);)

#define TEST_CACHED_UPLOADER(uploader) ((TestCachedUploader*) uploader)

static void
test_cached_uploader_reset_prev_stats()
{
  prev_test_cached_uploader_stats.upload_part_count = 0;
  prev_test_cached_uploader_stats.upload_copy_part_count = 0;
  prev_test_cached_uploader_stats.cache_hits = 0;
  prev_test_cached_uploader_stats.cache_misses = 0;
  uploaded_buffer.clear();
  uploader_buffer.clear();
}

static void
test_cached_uploader_destroy (GstS3Uploader * uploader)
{
  TestCachedUploader *inst = TEST_CACHED_UPLOADER(uploader);
  prev_test_cached_uploader_stats.upload_part_count = inst->upload_part_count;
  prev_test_cached_uploader_stats.upload_copy_part_count = inst->upload_copy_part_count;
  prev_test_cached_uploader_stats.cache_hits = inst->cache_hits;
  prev_test_cached_uploader_stats.cache_misses = inst->cache_misses;

  delete inst->cache;
  inst->cache = NULL;
  g_free(uploader);
}

static gboolean
test_cached_uploader_upload_part (
  GstS3Uploader * uploader,
  const gchar * buffer,
  gsize size,
  gchar **next,
  gsize *next_size)
{
  TestCachedUploader *inst = TEST_CACHED_UPLOADER(uploader);

  inst->upload_part_count++;
  inst->current_part_num++;

  // add this buffer (part) to the uploaded_buffer.
  uploader_buffer.write(buffer, size);

  inst->cache->get_copy(inst->current_part_num+1, next, next_size);
  if (*next != NULL)
    inst->cache_hits++;
  else if (*next_size > 0)
    inst->cache_misses++;
  inst->cache->insert_or_update(inst->current_part_num, buffer, size);

  return TRUE;
}

static gboolean
test_cached_uploader_upload_part_copy (GstS3Uploader * uploader, G_GNUC_UNUSED const gchar * bucket,
  G_GNUC_UNUSED const gchar * key, G_GNUC_UNUSED gsize first, G_GNUC_UNUSED gsize last)
{
  TEST_CACHED_UPLOADER(uploader)->upload_copy_part_count++;
  return TRUE;
}

static gboolean
test_cached_uploader_seek (GstS3Uploader *uploader, gsize offset, gchar **buffer, gsize *_size)
{
  TestCachedUploader *inst = TEST_CACHED_UPLOADER(uploader);

  gint part;
  if (inst->cache->find(offset, &part, buffer, _size)) {
    if (*buffer != NULL) {
      inst->cache_hits++;

      // -1 to mimic what the MPU does since it increments first on upload_part
      inst->current_part_num = part - 1;

      // the seek is relative to the part
      // assuming all parts are the same size
      gsize boffset = inst->current_part_num * inst->buffer_size;
      uploader_buffer.seekp (boffset, std::ios::beg);
      return TRUE;
    }
    else {
      inst->cache_misses++;
    }
  }
  else {
    inst->cache_misses++;
  }
  return FALSE;
}

static gboolean
test_cached_uploader_complete (G_GNUC_UNUSED GstS3Uploader * uploader)
{
  uploaded_buffer = uploader_buffer.str();
  uploader_buffer.clear();
  return TRUE;
}

static GstS3UploaderClass test_cached_uploader_class = {
  test_cached_uploader_destroy,
  test_cached_uploader_upload_part,
  test_cached_uploader_upload_part_copy,
  test_cached_uploader_seek,
  test_cached_uploader_complete
};

static GstS3Uploader*
test_cached_uploader_new (
  const GstS3UploaderConfig *config)
{
  TestCachedUploader *uploader = g_new(TestCachedUploader, 1);

  uploader->base.klass = &test_cached_uploader_class;
  uploader->upload_part_count = 0;
  uploader->upload_copy_part_count = 0;
  uploader->cache_hits = 0;
  uploader->cache_misses = 0;
  uploader->buffer_size = config->buffer_size;
  uploader->current_part_num = 0;
  uploader->cache = new UploaderPartCache(config->cache_num_parts);

  return (GstS3Uploader*) uploader;
}

typedef struct {
  GstS3Downloader base;
    gsize buffer_size;

  size_t bytes_downloaded;
  gint downloads_requested;
} TestCachedDownloader;

#define TEST_DOWNLOADER(downloader) ((TestCachedDownloader*) downloader)

// Simply returns that the amount requested was actually returned.
static size_t
test_cached_downloader_download_part (
  GstS3Downloader *downloader,
  char *buffer,
  size_t first,
  size_t last)
{
  auto inst = TEST_DOWNLOADER(downloader);
  size_t requested = last - first;

  inst->bytes_downloaded += requested;
  inst->downloads_requested++;

  if (requested > 0) {
    uploaded_buffer.copy(buffer, requested, first);
  }

  return requested;
}

static void
test_cached_downloader_destroy (GstS3Downloader *downloader)
{
  g_free(downloader);
}

static GstS3DownloaderClass test_cached_downloader_class = {
  test_cached_downloader_destroy,
  test_cached_downloader_download_part
};

static GstS3Downloader *
test_cached_downloader_new (const GstS3UploaderConfig *config)
{
  TestCachedDownloader *downloader = g_new (TestCachedDownloader, 1);
  downloader->base.klass = &test_cached_downloader_class;
  downloader->buffer_size = config->buffer_size;
  downloader->bytes_downloaded = 0;
  downloader->downloads_requested = 0;
  return (GstS3Downloader*) downloader;
}
