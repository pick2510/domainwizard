#pragma once

#include <stdexcept>
#include <string>

namespace wrftools {

class UserError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UnsupportedError : public UserError {
public:
    using UserError::UserError;
};

}  // namespace wrftools
