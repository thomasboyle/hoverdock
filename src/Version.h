#pragma once

// Single source of truth for the dock version and update feed.
// CMake injects DOCK_VERSION_STRING / DOCK_REPO_OWNER / DOCK_REPO_NAME via
// compile definitions; the fallbacks below keep IDE-only builds working.

#ifndef DOCK_VERSION_STRING
#define DOCK_VERSION_STRING "1.1.11"
#endif

#ifndef DOCK_REPO_OWNER
#define DOCK_REPO_OWNER "thomasboyle"
#endif

#ifndef DOCK_REPO_NAME
#define DOCK_REPO_NAME "hoverdock"
#endif

namespace DockVersion {

constexpr char kVersion[] = DOCK_VERSION_STRING;
constexpr char kRepoOwner[] = DOCK_REPO_OWNER;
constexpr char kRepoName[] = DOCK_REPO_NAME;

// ASCII marker kept in the binary so updates can verify the on-disk exe
// instead of trusting the installer registry alone.
constexpr char kVersionMarker[] = "HoverdockVersion=" DOCK_VERSION_STRING;

}  // namespace DockVersion
