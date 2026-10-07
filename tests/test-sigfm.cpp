// SIGFM storage regressions. SPDX-License-Identifier: LGPL-2.1-or-later
#include <glib.h>
#include "sigfm/binary.hpp"
#include "sigfm/img-info.hpp"
#include "sigfm/sigfm.hpp"

static void test_stream_bounds()
{
    bin::stream stream;
    stream << 42 << 19;
    int value;
    stream >> value;
    g_assert_cmpint(value, ==, 42);
    g_assert_cmpuint(stream.size(), ==, sizeof(int));
    stream >> value;
    g_assert_cmpint(value, ==, 19);
    bool rejected = false;
    try {
        stream >> value;
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    g_assert_true(rejected);
    g_assert_cmpuint(stream.size(), ==, 0);
}

static void test_matrix_roundtrip()
{
    cv::Mat source(4, 8, CV_32FC1);
    for (int y = 0; y < source.rows; ++y)
        for (int x = 0; x < source.cols; ++x)
            source.at<float>(y, x) = y * 10 + x;
    // A non-contiguous view must serialize only the selected rectangle.
    const cv::Mat view = source(cv::Rect(1, 1, 3, 2));
    bin::stream stream;
    stream << view;
    g_assert_cmpuint(stream.size(), ==, 3 * sizeof(int) + 6 * sizeof(float));
    cv::Mat restored;
    stream >> restored;
    g_assert_cmpint(restored.rows, ==, view.rows);
    g_assert_cmpint(restored.cols, ==, view.cols);
    g_assert_cmpint(restored.type(), ==, view.type());
    g_assert_cmpfloat(cv::norm(restored, view), ==, 0.0);
    g_assert_cmpuint(stream.size(), ==, 0);
}

static void test_invalid_matrix()
{
    const int headers[][3] = {
        {CV_32FC1, 2, 128}, // Missing descriptor bytes.
        {CV_8UC1, G_MAXINT, G_MAXINT},
        {CV_8UC1, -1, 2},
        {CV_8UC1, 1, -2},
        {-1, 1, 1},
        {CV_32FC1 | (1 << 20), 1, 1},
    };
    for (const auto& header : headers) {
        bin::stream stream;
        stream << header[0] << header[1] << header[2];
        bool rejected = false;
        try {
            cv::Mat matrix;
            stream >> matrix;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        g_assert_true(rejected);
    }
}

static void test_invalid_vector()
{
    bin::stream stream;
    stream << static_cast<std::size_t>(-1);
    bool rejected = false;
    try {
        std::vector<cv::KeyPoint> points;
        stream >> points;
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    g_assert_true(rejected);
}

static void test_template_roundtrip_and_truncation()
{
    SigfmImgInfo source;
    source.keypoints.emplace_back(12.0f, 34.0f, 8.0f);
    source.keypoints.emplace_back(42.0f, 21.0f, 6.0f);
    source.descriptors = cv::Mat::ones(2, 128, CV_32FC1);
    int length;
    g_autofree unsigned char* bytes = sigfm_serialize_binary(&source, &length);
    auto* restored = sigfm_deserialize_binary(bytes, length);
    g_assert_nonnull(restored);
    int copy_length;
    g_autofree unsigned char* copy = sigfm_serialize_binary(restored, &copy_length);
    g_assert_cmpint(copy_length, ==, length);
    g_assert_cmpmem(copy, copy_length, bytes, length);
    sigfm_free_info(restored);

    // Reject truncation at every boundary, including inside matrix data.
    for (int cut = 0; cut < length; ++cut)
        g_assert_null(sigfm_deserialize_binary(bytes, cut));
    g_assert_null(sigfm_deserialize_binary(nullptr, length));
    g_assert_null(sigfm_deserialize_binary(bytes, -1));

    std::vector<unsigned char> trailing(bytes, bytes + length);
    trailing.push_back(0);
    g_assert_null(sigfm_deserialize_binary(trailing.data(), trailing.size()));
}

static void test_same_row_matches()
{
    SigfmImgInfo a, b;
    a.descriptors = cv::Mat::zeros(6, 128, CV_32FC1);
    for (int i = 0; i < 6; ++i) {
        a.keypoints.emplace_back(float(10 * i), 10.0f, 2.0f);
        b.keypoints.emplace_back(float(10 * i + 5), 14.0f, 2.0f);
        a.descriptors.at<float>(i, i) = 1.0f;
    }
    b.descriptors = a.descriptors.clone();
    // Six distinct points on one row give 15 consistent segments, 105 pairs.
    g_assert_cmpint(sigfm_match_score(&a, &b), ==, 105);
    g_assert_cmpint(sigfm_match_score(nullptr, &b), ==, -1);
    g_assert_null(sigfm_extract(nullptr, 32, 32));
    g_assert_null(sigfm_extract(reinterpret_cast<const SfmPix*>(""), -1, 0));
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/sigfm/stream-bounds", test_stream_bounds);
    g_test_add_func("/sigfm/matrix-roundtrip", test_matrix_roundtrip);
    g_test_add_func("/sigfm/invalid-matrix", test_invalid_matrix);
    g_test_add_func("/sigfm/invalid-vector", test_invalid_vector);
    g_test_add_func("/sigfm/template-roundtrip-truncation", test_template_roundtrip_and_truncation);
    g_test_add_func("/sigfm/same-row-matches", test_same_row_matches);
    return g_test_run();
}
