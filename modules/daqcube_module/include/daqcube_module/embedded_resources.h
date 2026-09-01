/*
 * Resource IDs shared between the generated resources.rc and the extraction
 * code. The DAQCube module bundles the core host executable and the
 * game core dlls as RCDATA resources so the deliverable stays a single
 * self-contained module dll (only the openDAQ runtime ships separately).
 *
 * Included by the resource compiler - preprocessor definitions only.
 */

#pragma once

#define DAQGAME_RES_CORE_HOST 201
#define DAQGAME_RES_CORE_MRBOOM 202
#define DAQGAME_RES_CORE_SNES9X 203
#define DAQGAME_RES_CORE_PRBOOM 204
#define DAQGAME_RES_WAD_PRBOOM 205
#define DAQGAME_RES_WAD_FREEDOOM 206
