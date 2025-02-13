/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#raw
#pragma once

#include <array>
#include <fmt/format.h>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#end raw
##
##
#from bisect import bisect_left, bisect_right
#import itertools
#import re
#import yaml
#from packaging.version import Version
##
## `args[0]` : the path to a `releases.yml` file.
## `args[1]` : the mongo version
##
#set releases_yml_path = $args[0]
#set mongo_version = $args[1]
##
#set mvc_file = open(releases_yml_path, 'r')
#set mvc_doc = yaml.safe_load(mvc_file)
#set mvc_fcvs = mvc_doc['featureCompatibilityVersions']
#set mvc_majors = mvc_doc['longTermSupportReleases']
##
## Transform strings to versions.
#set global fcvs = list(map(Version, mvc_fcvs))
#set majors = list(map(Version, mvc_majors))

#set global latest = Version(re.match(r'^[0-9]+\.[0-9]+', $mongo_version).group(0))
## Highest release less than latest.
#set global last_continuous = $fcvs[bisect_left($fcvs, $latest) - 1]
## Highest LTS release less than latest.
#set global last_lts = majors[bisect_left(majors, $latest) - 1]
##
#set global generic_fcvs = {'LastLTS': $last_lts, 'LastContinuous': $last_continuous, 'Latest': $latest}
##
## Format a Version as `{major}_{minor}`.
#def underscores(v): ${'{}_{}'.format(v.major, v.minor)}
#def dotted(v): ${'{}.{}'.format(v.major, v.minor)}
#def fcv_prefix(v): ${'kFullyDowngradedTo_' if v == $last_lts else 'kVersion_'}
#def fcv_cpp_name(v): ${'{}{}'.format($fcv_prefix(v), $underscores(v))}
##
#def transition_enum_name(transition, first, second):
k$(transition)_$(underscores(first))_To_$(underscores(second))#slurp
#end def

namespace mongo::multiversion {
<%
fcvs = self.getVar('fcvs')
last_lts, last_continuous, latest = self.getVar('last_lts'), self.getVar('last_continuous'), self.getVar('latest')
generic_fcvs = self.getVar('generic_fcvs')

# The 'latest' version must be one of the versions listed in releases.yml.
assert (latest in fcvs)

# The transition when used as a cpp variable.
down = 'DowngradingFrom'
up = 'UpgradingFrom'

# TODO (SERVER-58333): Clean up this file to remove the generation and handling of FCV 4.x.
# The ordering of the FCV enums matters; the FCVs must appear in ascending order, ordered by version
# number, with the transition FCVs appearing interwoven between the version FCVs.
# A list of (FCV enum name, FCV string) tuples, for FCVs of the form 'X.Y'. Initialize with FCVs
# before the last_lts.
fcv_list = [(self.fcv_cpp_name(fcv), fcv) for fcv in fcvs[:bisect_left(fcvs, last_lts)]]

# A list of (FCV enum name, FCV string) tuples for all the transitioning FCV values.
transition_fcvs = []

for fcv_x in fcvs[bisect_left(fcvs, last_lts):bisect_right(fcvs, latest)]:
    fcv_list.append((self.fcv_cpp_name(fcv_x), self.dotted(fcv_x)))
    if fcv_x in generic_fcvs.values():
        up_transitions = []
        down_transitions = []
        for fcv_y in filter(lambda y : y > fcv_x, generic_fcvs.values()):
            up_transitions.append((self.transition_enum_name(up, fcv_x, fcv_y),
                            f'upgrading from {self.dotted(fcv_x)} to {self.dotted(fcv_y)}')) 
            down_transitions.append((self.transition_enum_name(down, fcv_y, fcv_x),
                            f'downgrading from {self.dotted(fcv_y)} to {self.dotted(fcv_x)}'))
        # The downgrading transitions need to appear first when generating enums.
        fcv_list.extend(down_transitions + up_transitions)
        transition_fcvs.extend(down_transitions + up_transitions)
%>
##
##
/**
 * The combination of the fields (version, targetVersion, previousVersion) in the
 * featureCompatibilityVersion document in the server configuration collection
 * (admin.system.version) are represented by this enum and determine this node's behavior.
 *
 * Features can be gated for specific versions, or ranges of versions above or below some
 * minimum or maximum version, respectively.
 *
 * While upgrading from version X to Y or downgrading from Y to X, the server supports the
 * features of the older of the two versions.
 *
 * For versions X and Y, the legal enums and featureCompatibilityVersion documents are:
 *
 * kFullyDowngradedTo_X
 * (X, Unset, Unset): Only version X features are available, and new and existing storage
 *                    engine entries use the X format
 *
 * kUpgradingFrom_X_To_Y
 * (X, Y, Unset): Only version X features are available, but new storage engine entries
 *                use the Y format, and existing entries may have either the X or
 *                Y format
 *
 * kVersion_X
 * (X, Unset, Unset): X features are available, and new and existing storage engine
 *                    entries use the X format
 *
 * kDowngradingFrom_X_To_Y
 * (Y, Y, X): Only Y features are available and new storage engine entries use the
 *            Y format, but existing entries may have either the Y or X format
 *
 * kUnsetDefaultLastLTSBehavior
 * (Unset, Unset, Unset): This is the case on startup before the fCV document is loaded into
 *                        memory. isVersionInitialized() will return false, and getVersion()
 *                        will return the default (kUnsetDefaultLastLTSBehavior).
 *
 */
enum class FeatureCompatibilityVersion {
    kInvalid,
    kUnsetDefaultLastLTSBehavior,

#for fcv, _ in fcv_list:
    $fcv,
#end for
};

## Calculate number of versions since v4.4.
constexpr size_t kSince_$underscores(Version('4.4')) = ${bisect_left(fcvs, latest)};

// Last LTS was "$last_lts".
constexpr size_t kSinceLastLTS = ${bisect_left(fcvs, latest) - bisect_left(fcvs, last_lts)};

constexpr inline StringData kParameterName = "featureCompatibilityVersion"_sd;

class GenericFCV {
#def define_fcv_alias(id, v):
static constexpr auto $id = FeatureCompatibilityVersion::$fcv_cpp_name(v);#slurp
#end def
##
#def define_generic_transition_alias(transition, first, second):
static constexpr auto k$transition$(first)To$(second) = #slurp
FeatureCompatibilityVersion::$transition_enum_name(transition, $generic_fcvs[first], $generic_fcvs[second]);#slurp
#end def
##
#def define_generic_invalid_alias(transition, first, second):
static constexpr auto k$transition$(first)To$(second) = FeatureCompatibilityVersion::kInvalid;#slurp
#end def
##
<%
lts = 'LastLTS'
cont = 'LastContinuous'
lat = 'Latest'
generic_transitions = [
    # LastLTS <=> Latest
    self.define_generic_transition_alias(up, lts, lat),
    self.define_generic_transition_alias(down, lat, lts),
    # LastContinuous <=> Latest
    self.define_generic_transition_alias(up, cont, lat),
    self.define_generic_transition_alias(down, lat, cont),
    # LastLTS => LastContinuous, when LastLTS != LastContinuous
    self.define_generic_transition_alias(up, lts, cont) if generic_fcvs[lts] != generic_fcvs[cont]
        else self.define_generic_invalid_alias(up, lts, cont)
]
%>
public:
    $define_fcv_alias('kLatest', latest)
    $define_fcv_alias('kLastContinuous', last_continuous)
    $define_fcv_alias('kLastLTS', last_lts)

#for fcv in generic_transitions:
    $fcv
#end for
};

/**
 * A table with mappings between versions of the type 'X.Y', as well as versions that are not of the
 * type 'X.Y' (i.e. the transition FCVs, 'kInvalid', and 'kUnsetDefaultLastLTSBehavior') and their
 * corresponding strings.
 */
inline constexpr std::array extendedFCVTable {
    // The table's entries must appear in the same order in which the enums were defined.
    std::pair{FeatureCompatibilityVersion::kInvalid, "invalid"_sd},
    std::pair{FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior, "unset"_sd},
#for fcv, fcv_string in fcv_list:
    std::pair{FeatureCompatibilityVersion::$fcv, "$fcv_string"_sd},
#end for
};

/**
 * A table with mappings between FeatureCompatibilityVersion::kVersion_X_Y and a pointer to the
 * "X.Y"_sd generated by the extended table. Thus, this a subset of the extended table.
 */
inline constexpr std::array standardFCVTable {
#for fcv, _ in filter(lambda p: p not in transition_fcvs, fcv_list):
    std::pair{FeatureCompatibilityVersion::$fcv,
        &extendedFCVTable[static_cast<size_t>(FeatureCompatibilityVersion::$fcv)].second},
#end for
};

constexpr StringData toString(FeatureCompatibilityVersion v) {
    return extendedFCVTable[static_cast<size_t>(v)].second;
}

/**
 * Parses 'versionString', of the form "X.Y", to its corresponding FCV enum. For example, "5.1"
 * will be parsed as FeatureCompatibilityVersion::$fcv_cpp_name(Version("5.1")).
 * Throws 'ErrorCodes::BadValue' when 'versionString' is not of the form "X.Y", and has no matching
 * enum.
 */
inline FeatureCompatibilityVersion parseVersionForFeatureFlags(StringData versionString) {
    for (const auto& [fcv, record] : standardFCVTable) {
        if (*record == versionString)
            return fcv;
    }

    uasserted(ErrorCodes::BadValue,
              fmt::format("Invalid FCV version {} for feature flag.", versionString));
}

/*
 * Returns whether the given FCV is a standard FCV enum, i.e. an enum that corresponds to a numbered
 * version of the form 'X.Y'. Non-standard enums are transition enums, 'kInvalid' and
 * 'kUnsetDefaultLastLTSBehavior'.
 */
inline bool isStandardFCV(FeatureCompatibilityVersion v) {
    for (const auto& [fcv, record] : standardFCVTable) {
        if (v == fcv)
            return true;
    }

    return false;
}

}  // namespace mongo::multiversion

/* vim: set filetype=cpp: */
