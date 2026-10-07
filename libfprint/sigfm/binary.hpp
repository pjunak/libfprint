
#pragma once

#include "opencv2/core/mat.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bin {
using byte = unsigned char;

class stream;

template<typename T>
struct serializer : public std::false_type {
    void serialize(const T& m, stream& out);
};

template<typename T>
struct deserializer : public std::false_type {
    T deserialize(stream& in);
};
class stream {
public:
    stream() = default;

    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    stream(Iter begin, Iter end) : store_{begin, end}
    {
    }

    template<typename T, std::enable_if_t<serializer<T>::value, bool> = true>
    constexpr stream& operator<<(const T& v)
    {
        serializer<T>::serialize(v, *this);
        return *this;
    }

    template<typename T, std::enable_if_t<deserializer<T>::value, bool> = true>
    constexpr stream& operator>>(T& v)
    {
        v = deserializer<T>::deserialize(*this);
        return *this;
    }
    template<typename T, std::enable_if_t<std::is_trivial_v<T>, bool> = true>
    constexpr stream& operator<<(T v)
    {
        using seg_store = std::array<byte, sizeof(T)>;
        alignas(T) seg_store s = {};
        std::memcpy(s.data(), &v, sizeof(T));
        stream::write(s.begin(), s.end());
        return *this;
    }

    template<typename T, std::enable_if_t<std::is_trivial_v<T>, bool> = true>
    constexpr stream& operator>>(T& v)
    {
        using seg_store = std::array<byte, sizeof(T)>;
        alignas(T) seg_store s = {};
        stream::read(s.begin(), s.end());
        memcpy(&v, s.data(), sizeof(T));
        return *this;
    }
    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    constexpr stream& write(Iter&& begin, Iter&& end)
    {
        std::copy(std::forward<Iter>(begin), std::forward<Iter>(end),
                  std::back_inserter(store_));
        return *this;
    }

    template<typename T, std::enable_if_t<serializer<T>::value, bool> = true>
    stream& serialize(const T& m, stream& out)
    {
        serializer<T>::serialize(m, out);
        return out;
    }

    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    constexpr stream& read(Iter&& begin, Iter&& end)
    {
        const auto dist = std::distance(begin, end);
        return stream::read(begin, dist);
    }

    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    constexpr stream& read(Iter&& begin, std::size_t dist)
    {
        if (dist > size()) {
            throw std::runtime_error{"truncated binary data"};
        }
        std::copy_n(store_.begin() + read_offset_, dist, begin);
        read_offset_ += dist;
        return *this;
    }
    byte* copy_buffer() const
    {
        byte* raw = static_cast<byte*>(malloc(size()));
        if (!raw && size() != 0) {
            throw std::bad_alloc{};
        }
        std::copy(store_.begin() + read_offset_, store_.end(), raw);
        return raw;
    }
    std::size_t size() const { return store_.size() - read_offset_; }

private:
    std::vector<byte> store_;
    std::size_t read_offset_ = 0;
};

template<>
struct serializer<cv::Mat> : public std::true_type {
    static void serialize(const cv::Mat& m, stream& out)
    {
        out << m.type() << m.rows << m.cols;
        // A matrix can be a view into a larger allocation. Store just its rows.
        const auto row_bytes = static_cast<std::size_t>(m.cols) * m.elemSize();
        for (int row = 0; row < m.rows; ++row) {
            out.write(m.ptr(row), m.ptr(row) + row_bytes);
        }
    }
};

template<>
struct deserializer<cv::Mat> : public std::true_type {
    static cv::Mat deserialize(stream& in)
    {
        int rows, cols, type;
        in >> type >> rows >> cols;
        if (rows < 0 || cols < 0 || type < 0 || type != CV_MAT_TYPE(type)) {
            throw std::runtime_error{"invalid matrix dimensions or type"};
        }
        // Check against the available input before allocating. Division avoids
        // overflow when the dimensions came from a damaged template.
        const auto element_size = CV_ELEM_SIZE(type);
        if (rows != 0 && static_cast<std::size_t>(cols) > in.size() / element_size) {
            throw std::runtime_error{"truncated matrix row"};
        }
        const auto row_bytes = static_cast<std::size_t>(cols) * element_size;
        if (row_bytes != 0 && static_cast<std::size_t>(rows) > in.size() / row_bytes) {
            throw std::runtime_error{"truncated matrix data"};
        }
        cv::Mat m;
        m.create(rows, cols, type);
        in.read(m.data, static_cast<std::size_t>(rows) * row_bytes);
        return m;
    }
};

template<typename T>
struct deserializer<cv::Point_<T>> : public std::true_type {
    static cv::Point_<T> deserialize(stream& in)
    {
        cv::Point_<T> p;
        in >> p.x >> p.y;
        return p;
    }
};
template<typename T>
struct serializer<cv::Point_<T>> : public std::true_type {
    static void serialize(const cv::Point_<T>& pt, stream& out)
    {
        out << pt.x << pt.y;
    }
};

template<>
struct serializer<cv::KeyPoint> : public std::true_type {
    static void serialize(const cv::KeyPoint& pt, stream& out)
    {
        out << pt.class_id << pt.angle << pt.octave << pt.response << pt.size
            << pt.pt;
    }
};

template<>
struct deserializer<cv::KeyPoint> : public std::true_type {
    static cv::KeyPoint deserialize(stream& in)
    {
        cv::KeyPoint pt;
        in >> pt.class_id >> pt.angle >> pt.octave >> pt.response >> pt.size >>
            pt.pt;
        return pt;
    }
};

template<typename T>
struct serializer<std::vector<T>> : public std::true_type {
    static void serialize(const std::vector<T>& vs, stream& out)
    {
        out << static_cast<std::size_t>(vs.size());
        std::for_each(vs.begin(), vs.end(),
                      [&out](const auto& el) { out << el; });
    }
};

template<typename T>
struct deserializer<std::vector<T>> : public std::true_type {
    static std::vector<T> deserialize(stream& in)
    {
        std::size_t size;
        in >> size;
        if (size > in.size()) {
            throw std::runtime_error{"invalid vector length"};
        }
        std::vector<T> vs;
        for (std::size_t n = 0; n != size; ++n) {
            T v;
            in >> v;
            vs.emplace_back(std::move(v));
        }
        return vs;
    }
};
} // namespace bin
