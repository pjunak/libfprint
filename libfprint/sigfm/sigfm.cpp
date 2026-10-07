// SIGFM algorithm for libfprint

// Copyright (C) 2022 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (c) 2022 Natasha England-Elbro <ashenglandelbro@protonmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
//

#include "sigfm.hpp"
#include "binary.hpp"
#include "img-info.hpp"

#include "opencv2/features2d.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

namespace bin {

template<>
struct serializer<SigfmImgInfo> : public std::true_type {
    static void serialize(const SigfmImgInfo& info, stream& out)
    {
        out << info.keypoints << info.descriptors;
    }
};

template<>
struct deserializer<SigfmImgInfo> : public std::true_type {
    static SigfmImgInfo deserialize(stream& in)
    {
        SigfmImgInfo info;
        in >> info.keypoints >> info.descriptors;
        return info;
    }
};
} // namespace bin

namespace {
constexpr auto distance_match = 0.75;
constexpr auto length_match = 0.05;
constexpr auto angle_match = 0.05;
constexpr auto min_match = 5;
struct match {
    cv::Point2i p1;
    cv::Point2i p2;
    match(cv::Point2i ip1, cv::Point2i ip2) : p1{ip1}, p2{ip2} {}
    match() : p1{cv::Point2i(0, 0)}, p2{cv::Point2i(0, 0)} {}
    bool operator==(const match& right) const
    {
        return std::tie(this->p1, this->p2) == std::tie(right.p1, right.p2);
    }
    bool operator<(const match& right) const
    {
        return std::tie(p1.y, p1.x, p2.y, p2.x) <
               std::tie(right.p1.y, right.p1.x, right.p2.y, right.p2.x);
    }
};
bool angle_close(double a, double b)
{
    const double high = std::max(a, b);
    return high == 0 || 1 - std::min(a, b) / high <= angle_match;
}
struct angle {
    double cos;
    double sin;
    match corr_matches[2];
    angle(double cos_, double sin_, match m1, match m2)
        : cos{cos_}, sin{sin_}, corr_matches{m1, m2}
    {
    }
};
} // namespace

SigfmImgInfo* sigfm_copy_info(SigfmImgInfo* info)
{
    try { return info ? new SigfmImgInfo{*info} : nullptr; }
    catch (...) { return nullptr; }
}

int sigfm_keypoints_count(SigfmImgInfo* info) { return info ? info->keypoints.size() : 0; }
unsigned char* sigfm_serialize_binary(SigfmImgInfo* info, int* outlen)
{
    if (!outlen) return nullptr;
    *outlen = 0;
    if (!info) return nullptr;
    try {
        bin::stream s;
        s << *info;
        if (s.size() > std::numeric_limits<int>::max()) return nullptr;
        auto* bytes = s.copy_buffer();
        if (bytes) *outlen = s.size();
        return bytes;
    } catch (...) { return nullptr; }
}

SigfmImgInfo* sigfm_deserialize_binary(const unsigned char* bytes, int len)
{
    if (!bytes || len <= 0) {
        return nullptr;
    }
    try {
        bin::stream s{bytes, bytes + len};
        auto info = std::make_unique<SigfmImgInfo>();
        s >> *info;
        if (s.size() != 0) {
            return nullptr;
        }
        return info.release();
    }
    catch (const std::exception&) {
        return nullptr;
    }
}

SigfmImgInfo* sigfm_extract(const SfmPix* pix, int width, int height)
{
    if (!pix || width <= 0 || height <= 0) return nullptr;
    try {
    cv::Mat img;
    img.create(height, width, CV_8UC1);
    std::memcpy(img.data, pix, static_cast<std::size_t>(width) * height);
    const auto roi = cv::Mat::ones(cv::Size{img.size[1], img.size[0]}, CV_8UC1);
    std::vector<cv::KeyPoint> pts;

    cv::Mat descs;
    cv::SIFT::create()->detectAndCompute(img, roi, pts, descs);

    auto* info = new SigfmImgInfo{pts, descs};
    return info;
    } catch (...) { return nullptr; }
}

int sigfm_match_score(SigfmImgInfo* frame, SigfmImgInfo* enrolled)
{
    if (!frame || !enrolled) return -1;
    if (frame->descriptors.empty() || enrolled->descriptors.rows < 2) return 0;
    try {
        std::vector<std::vector<cv::DMatch>> points;
        auto bfm = cv::BFMatcher::create();
        bfm->knnMatch(frame->descriptors, enrolled->descriptors, points, 2);
        std::set<match> matches_unique;

        for (const auto& pts : points) {
            if (pts.size() < 2) {
                continue;
            }
            const cv::DMatch& match_1 = pts.at(0);
            if (match_1.distance < distance_match * pts.at(1).distance) {
                matches_unique.emplace(
                    match{frame->keypoints.at(match_1.queryIdx).pt,
                          enrolled->keypoints.at(match_1.trainIdx).pt});

            }
        }
        if (matches_unique.size() < min_match) {
            return 0;
        }
        std::vector<match> matches{matches_unique.begin(),
                                   matches_unique.end()};

        std::vector<angle> angles;
        for (std::size_t j = 0; j < matches.size(); j++) {
            match match_1 = matches[j];
            for (std::size_t k = j + 1; k < matches.size(); k++) {
                match match_2 = matches[k];

                double vec_1[2] = {double(match_1.p1.x) - match_2.p1.x,
                                double(match_1.p1.y) - match_2.p1.y};
                double vec_2[2] = {double(match_1.p2.x) - match_2.p2.x,
                                double(match_1.p2.y) - match_2.p2.y};

                double length_1 = sqrt(pow(vec_1[0], 2) + pow(vec_1[1], 2));
                double length_2 = sqrt(pow(vec_2[0], 2) + pow(vec_2[1], 2));

                if (length_1 == 0 || length_2 == 0) continue;
                if (1 - std::min(length_1, length_2) /
                            std::max(length_1, length_2) <=
                    length_match) {

                    double product = length_1 * length_2;
                    angles.emplace_back(angle(
                        M_PI / 2 +
                            asin(std::clamp((vec_1[0] * vec_2[0] + vec_1[1] * vec_2[1]) /
                                 product, -1.0, 1.0)),
                        acos(std::clamp((vec_1[0] * vec_2[1] - vec_1[1] * vec_2[0]) /
                             product, -1.0, 1.0)),
                        match_1, match_2));
                }
            }
        }

        if (angles.size() < min_match) {
            return 0;
        }

        int count = 0;
        for (std::size_t j = 0; j < angles.size(); j++) {
            angle angle_1 = angles[j];
            for (std::size_t k = j + 1; k < angles.size(); k++) {
                angle angle_2 = angles[k];

                if (angle_close(angle_1.sin, angle_2.sin) &&
                    angle_close(angle_1.cos, angle_2.cos)) {

                    if (count == std::numeric_limits<int>::max()) return count;
                    count += 1;
                }
            }
        }
        return count;
    }
    catch (...) {
        return -1;
    }
}

void sigfm_free_info(SigfmImgInfo* info) { delete info; }
