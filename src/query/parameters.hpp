#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "query/typed_value.hpp"

/**
 * Encapsulates user provided parameters (and stripped literals)
 * and provides ways of obtaining them by position.
 */
// TODO move to namespace query::
namespace query {

struct Parameters {
 public:
  /**
   * Adds a value to the stripped arguments under a token position.
   *
   * @param position Token position in query of value.
   * @param value
   */
  void Add(int position, const query::TypedValue &value) {
    storage_.emplace_back(position, value);
  }

  /**
   *  Returns the value found for the given token position.
   *
   *  @param position Token position in query of value.
   *  @return Value for the given token position.
   */
  const query::TypedValue &AtTokenPosition(int position) const {
    auto found = std::find_if(storage_.begin(), storage_.end(),
                              [&](const std::pair<int, query::TypedValue> a) {
                                return a.first == position;
                              });
    CHECK(found != storage_.end())
        << "Token position must be present in container";
    return found->second;
  }

  /**
   * Returns the position-th stripped value. Asserts that this
   * container has at least (position + 1) elements.
   *
   * @param position Which stripped param is sought.
   * @return Token position and value for sought param.
   */
  const std::pair<int, query::TypedValue> &At(int position) const {
    CHECK(position < static_cast<int>(storage_.size())) << "Invalid position";
    return storage_[position];
  }

  /** Returns the number of arguments in this container */
  auto size() const { return storage_.size(); }

  auto begin() const { return storage_.begin(); }
  auto end() const { return storage_.end(); }

 private:
  std::vector<std::pair<int, query::TypedValue>> storage_;
};

}  // namespace query
