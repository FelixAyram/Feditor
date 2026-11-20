#pragma once

#include "omsi/tt_types.hpp"

#include <filesystem>
#include <string>

namespace omsi::tt {

// Load entire maps/<Name>/TTData (Busstops.cfg, StnLinks.cfg, *.ttr, *.ttp, *.ttl).
// map_version is the map file version (OMSI gvar+0x20). < 11 => legacy short record layouts and no
// Busstops.cfg / StnLinks.cfg.
[[nodiscard]] bool load_ttdata(const std::filesystem::path& ttdata_dir, TimeTableData& out,
                               std::int32_t map_version = kTtExtendedVersion);

// Individual loaders (also used by load_ttdata).
[[nodiscard]] bool load_busstops_cfg(const std::filesystem::path& path, std::vector<BusstopListEntry>& out);
[[nodiscard]] bool load_stnlinks_cfg(const std::filesystem::path& path, std::vector<StnLink>& out);
[[nodiscard]] bool load_track_ttr(const std::filesystem::path& path, Track& out,
                                  std::int32_t map_version = kTtExtendedVersion);
[[nodiscard]] bool load_trip_ttp(const std::filesystem::path& path, Trip& out,
                                 std::int32_t map_version = kTtExtendedVersion);
[[nodiscard]] bool load_line_ttl(const std::filesystem::path& path, Line& out);

// Save (OMSI write order — CRLF, one value per line after [tag]).
[[nodiscard]] bool save_busstops_cfg(const std::filesystem::path& path, const std::vector<BusstopListEntry>& list);
[[nodiscard]] bool save_stnlinks_cfg(const std::filesystem::path& path, const std::vector<StnLink>& links);
[[nodiscard]] bool save_track_ttr(const Track& track);
[[nodiscard]] bool save_trip_ttp(const Trip& trip);
[[nodiscard]] bool save_line_ttl(const Line& line);

// Persist whole TimeTableData into root. Like TTimeTableMan.Save it also removes *.ttr/*.ttp/*.ttl
// files that no longer exist in memory (after the new files were written successfully).
[[nodiscard]] bool save_ttdata(const TimeTableData& data);

// Copy a saved TTData folder to another map folder (original map / "cambios"), including the
// removal of tracks/trips/lines that were deleted. Used by Ctrl+S when the live map is written too.
bool mirror_ttdata(const std::filesystem::path& src_ttdata, const std::filesystem::path& dst_ttdata);

// Build TrackEntry from a live editor path pick (what Add Trackentry / Add Path Segments stores).
[[nodiscard]] TrackEntry make_track_entry_from_path(std::int32_t owner_id, std::int32_t path_index,
                                                   std::int32_t tile_index, std::int32_t internal_path_no,
                                                   float length);

// Validate tracks/trips/links; fills data.errors (Find Errors on loaded tiles).
void validate_ttdata(TimeTableData& data);

} // namespace omsi::tt
