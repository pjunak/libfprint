/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "sigfm.hpp"
#include <stddef.h>

SigfmImgInfo *sigfm_extract (const SfmPix *pixels, int width, int height) { return NULL; }
void sigfm_free_info (SigfmImgInfo *info) { }
int sigfm_match_score (SigfmImgInfo *frame, SigfmImgInfo *enrolled) { return -1; }
unsigned char *sigfm_serialize_binary (SigfmImgInfo *info, int *length)
{
  if (length) *length = 0;
  return NULL;
}
SigfmImgInfo *sigfm_deserialize_binary (const unsigned char *bytes, int length) { return NULL; }
int sigfm_keypoints_count (SigfmImgInfo *info) { return 0; }
SigfmImgInfo *sigfm_copy_info (SigfmImgInfo *info) { return NULL; }
