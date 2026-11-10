/**
 * @file binding.h
 * @brief Structured DTO binding integration and Context::bind_json bridge for Kale.
 */
#ifndef KALE_BINDING_H
#define KALE_BINDING_H

#include <kale/http/field_desc.h>
#include <kale/http/json_reader.h>
#include <kale/core/context.h>
#include <kale/http/json.h>

namespace kale {

#ifndef KALE_CONTEXT_BIND_JSON_DEFINED
#define KALE_CONTEXT_BIND_JSON_DEFINED
template <typename DTO, typename J>
inline DTO Context::bind_json() const {
    return bind_json_body<DTO, J>(req_body());
}
#endif

} // namespace kale

#endif // KALE_BINDING_H
