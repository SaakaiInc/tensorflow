/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/framework/tensor_shape.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {

// An upper limit of the total number of elements in a tensor.
static const int64 kMaxElements = (1LL << 40);

static void AppendTo(const TensorShape& s, gtl::InlinedVector<int64, 8>* vals) {
  for (auto it = s.begin(); it != s.end(); ++it) {
    vals->push_back((*it).size);
  }
}

bool TensorShape::IsValid(const TensorShapeProto& proto) {
  int64 num_elements = 1;
  for (const auto& d : proto.dim()) {
    if (d.size() < 0) return false;
    num_elements *= d.size();
    if (num_elements > kMaxElements) return false;
  }
  return true;
}

Status TensorShape::IsValidShape(const TensorShapeProto& proto) {
  int64 num_elements = 1;
  for (const auto& d : proto.dim()) {
    if (d.size() < 0) {
      return errors::InvalidArgument("Shape ", DebugString(proto),
                                     " has negative dimensions");
    }
    num_elements *= d.size();
    if (num_elements > kMaxElements) {
      return errors::InvalidArgument("Shape ", DebugString(proto),
                                     " is too large (more than ", kMaxElements,
                                     " entries)");
    }
  }
  return Status::OK();
}

TensorShape::TensorShape(const TensorShapeProto& proto) {
  set_tag(REP16);
  set_ndims_byte(0);
  num_elements_ = 1;
  for (const auto& d : proto.dim()) {
    AddDim(d.size());
  }
}

TensorShape::TensorShape(gtl::ArraySlice<int64> dim_sizes) {
  set_tag(REP16);
  set_ndims_byte(0);
  num_elements_ = 1;
  for (auto s : dim_sizes) {
    AddDim(s);
  }
}

TensorShape::TensorShape() {
  set_tag(REP16);
  set_ndims_byte(0);
  num_elements_ = 1;
}

void TensorShape::SlowCopyFrom(const TensorShape& b) {
  if (b.tag() != REP_OUT_OF_LINE) {
    if (tag() == REP_OUT_OF_LINE) {
      delete as64()->dims_;
    }
    memcpy(buf(), b.buf(), sizeof(u_.buf));
    // memcpy above implicitly also does:
    //   set_tag(b.tag());
    //   set_ndims_byte(b.ndims_byte());
  } else {
    DCHECK_EQ(b.tag(), REP_OUT_OF_LINE);
    set_ndims_byte(b.ndims_byte());
    if (tag() == REP_OUT_OF_LINE) {
      // vector already allocated
      *(as64()->dims_) = *(b.as64()->dims_);
    } else {
      set_tag(REP_OUT_OF_LINE);
      as64()->dims_ = new gtl::InlinedVector<int64, 4>(*(b.as64()->dims_));
    }
  }
}

void TensorShape::Clear() {
  if (tag() == REP_OUT_OF_LINE) {
    delete as64()->dims_;
  }
  set_tag(REP16);
  set_ndims_byte(0);
  num_elements_ = 1;
}

void TensorShape::RecomputeNumElements() {
  int64 n = 1;
  for (auto it = begin(); it != end(); ++it) {
    n *= (*it).size;
    CHECK_LE(0, n);
    CHECK_LE(n, kMaxElements);
  }
  num_elements_ = n;
}

void TensorShape::AddDim(int64 size) {
  CHECK_GE(size, 0);
  const int nd = ndims_byte();
  CHECK_LT(nd, 255) << "Too many dimensions in tensor";
  if (tag() == REP16 && nd < 7 && size < kMaxRep16) {
    as16()->dims_[nd] = static_cast<int16>(size);
  } else if (tag() == REP32 && nd < 3 && size < kMaxRep32) {
    as32()->dims_[nd] = static_cast<int32>(size);
  } else if (tag() == REP_OUT_OF_LINE) {
    as64()->dims_->push_back(size);
  } else {
    // Need to change representation
    gtl::InlinedVector<int64, 8> vals;
    AppendTo(*this, &vals);
    vals.push_back(size);
    // We know we can't be REP16.  See if we have a small enough
    // number of dimensions and each dimension's size is small enough
    // to allow REP32.
    bool can_be_rep32 = (vals.size() <= 3);
    if (can_be_rep32) {
      for (size_t i = 0; i < vals.size(); i++) {
        if (vals[i] >= kMaxRep32) {
          can_be_rep32 = false;
          break;
        }
      }
    }
    if (can_be_rep32) {
      set_tag(REP32);
      for (size_t d = 0; d < vals.size(); d++) {
        as32()->dims_[d] = static_cast<int32>(vals[d]);
      }
    } else {
      set_tag(REP_OUT_OF_LINE);
      as64()->dims_ =
          new gtl::InlinedVector<int64, 4>(vals.begin(), vals.end());
    }
  }
  set_ndims_byte(nd + 1);
  num_elements_ *= size;
  CHECK_LE(0, num_elements_);
  CHECK_LE(num_elements_, kMaxElements);
}

void TensorShape::AppendShape(const TensorShape& shape) {
  for (auto d : shape) AddDim(d.size);
}

void TensorShape::InsertDim(int d, int64 size) {
  CHECK_GE(d, 0);
  CHECK_LE(d, dims());
  CHECK_GE(size, 0);
  gtl::InlinedVector<int64, 8> vals;
  AppendTo(*this, &vals);
  vals.insert(vals.begin() + d, size);
  Clear();
  for (auto dval : vals) {
    AddDim(dval);
  }
}

gtl::InlinedVector<int64, 4> TensorShape::dim_sizes() const {
  gtl::InlinedVector<int64, 4> result;
  for (auto it = begin(); it != end(); ++it) {
    result.push_back((*it).size);
  }
  return result;
}

void TensorShape::set_dim(int d, int64 size) {
  CHECK_GE(d, 0);
  CHECK_LT(d, dims());
  CHECK_GE(size, 0);
  if (tag() == REP16 && size < kMaxRep16) {
    as16()->dims_[d] = static_cast<int16>(size);
  } else if (tag() == REP32 && size < kMaxRep32) {
    as32()->dims_[d] = static_cast<int32>(size);
  } else if (tag() == REP_OUT_OF_LINE) {
    (*as64()->dims_)[d] = size;
  } else {
    // Must upgrade
    gtl::InlinedVector<int64, 8> vals;
    AppendTo(*this, &vals);
    vals[d] = size;
    Clear();
    for (auto dval : vals) {
      AddDim(dval);
    }
  }
  RecomputeNumElements();
}

void TensorShape::RemoveDim(int d) {
  CHECK_GE(d, 0);
  CHECK_LT(d, dims());
  gtl::InlinedVector<int64, 8> vals;
  AppendTo(*this, &vals);
  vals.erase(vals.begin() + d);
  Clear();
  for (auto dval : vals) {
    AddDim(dval);
  }
  RecomputeNumElements();
}

bool TensorShape::IsSameSize(const TensorShape& b) const {
  if (b.dims() != dims()) return false;
  for (int d = 0; d < dims(); d++) {
    if (dim_size(d) != b.dim_size(d)) return false;
  }
  return true;
}

void TensorShape::AsProto(TensorShapeProto* proto) const {
  proto->Clear();
  for (auto d = begin(); d != end(); ++d) {
    auto* dim = proto->add_dim();
    dim->set_size((*d).size);
  }
}

void TensorShape::DumpRep() const {
#if 0
  fprintf(stderr, "Rep: %d %d dims\n", tag(), dims());
  if (tag() == REP16) {
    fprintf(stderr, "REP16 NDIMS: %d\n", ndims_byte());
    for (int i = 0; i < ndims_byte(); i++) {
      fprintf(stderr, "dim %d: %d\n", i, as16()->dims_[i]);
    }
  } else if (tag_ == REP32) {
    fprintf(stderr, "REP32 NDIMS: %d\n", ndims_);
    for (int i = 0; i < ndims_byte(); i++) {
      fprintf(stderr, "dim %d: %d\n", i, as32()->dims_[i]);
    }
  } else if (tag_ == REP_OUT_OF_LINE) {
    fprintf(stderr, "REP_OUT_OF_LINE NDIMS: %d %p\n", ndims_, as16()->dims_);
    for (int i = 0; i < ndims_byte(); i++) {
      fprintf(stderr, "dim %d: %lld\n", i, (*as64()->dims_)[i]);
    }
  }
#endif
}

TensorShapeIter TensorShape::begin() const { return TensorShapeIter(this, 0); }

TensorShapeIter TensorShape::end() const {
  return TensorShapeIter(this, dims());
}

string TensorShape::DebugString() const {
  gtl::InlinedVector<int64, 8> vals;
  AppendTo(*this, &vals);
  return strings::StrCat("[", str_util::Join(gtl::ArraySlice<int64>(vals), ","),
                         "]");
}

string TensorShape::DebugString(const TensorShapeProto& proto) {
  string s = "[";
  bool first = true;
  for (const auto& d : proto.dim()) {
    strings::StrAppend(&s, first ? "" : ",", d.size());
    first = false;
  }
  strings::StrAppend(&s, "]");
  return s;
}

bool TensorShapeUtils::StartsWith(const TensorShape& shape,
                                  const TensorShape& prefix) {
  if (shape.dims() < prefix.dims()) return false;
  for (int i = 0; i < prefix.dims(); i++) {
    if (shape.dim_size(i) != prefix.dim_size(i)) return false;
  }
  return true;
}

}  // namespace tensorflow
