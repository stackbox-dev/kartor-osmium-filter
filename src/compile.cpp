
#include <cassert>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <regex>

#include "object_filter.hpp"

#include "compiled_filter.hpp"

namespace detail {

    int get_type(const osmium::OSMObject& object) noexcept {
        return int(object.type());
    }

    std::int64_t get_id(const osmium::OSMObject& object) noexcept {
        return int64_t(object.id());
    }

    std::int64_t get_uid(const osmium::OSMObject& object) noexcept {
        return int64_t(object.uid());
    }

    std::int64_t get_version(const osmium::OSMObject& object) noexcept {
        return int64_t(object.version());
    }

    std::int64_t get_changeset(const osmium::OSMObject& object) noexcept {
        return int64_t(object.changeset());
    }

    std::int64_t get_count_tags(const osmium::OSMObject& object) noexcept {
        return int64_t(object.tags().size());
    }

    std::int64_t get_count_nodes(const osmium::OSMObject& object) noexcept {
        if (object.type() != osmium::item_type::way) {
            return 0;
        }
        return int64_t(static_cast<const osmium::Way&>(object).nodes().size());
    }

    std::int64_t get_count_members(const osmium::OSMObject& object) noexcept {
        if (object.type() != osmium::item_type::relation) {
            return 0;
        }
        return int64_t(static_cast<const osmium::Relation&>(object).members().size());
    }

    bool has_key(const osmium::OSMObject& object, const char* key) {
        return object.tags().has_key(key);
    }

    bool check_tag_equals(const osmium::OSMObject& object, const char* key, const char* value) noexcept {
        const char* tag_value = object.tags().get_value_by_key(key);
        return tag_value && !std::strcmp(value, tag_value);
    }

    bool check_tag_not_equals(const osmium::OSMObject& object, const char* key, const char* value) noexcept {
        const char* tag_value = object.tags().get_value_by_key(key);
        return tag_value && std::strcmp(value, tag_value);
    }

    bool check_tag_match(const osmium::OSMObject& object, const char* key, const std::regex* value) noexcept {
        const char* tag_value = object.tags().get_value_by_key(key);
        return tag_value && std::regex_search(tag_value, *value);
    }

    bool check_tag_not_match(const osmium::OSMObject& object, const char* key, const std::regex* value) noexcept {
        const char* tag_value = object.tags().get_value_by_key(key);
        return tag_value && !std::regex_search(tag_value, *value);
    }

} // namespace detail

NativeJIT::Node<bool>& CompiledFilter::compile_and(const AndExpr* e) {
    assert(e->children().size() >= 2);
    auto f = e->children().begin();
    auto l = e->children().end();
    const auto* e1 = *f;
    ++f;
    const auto* e2 = *f;
    ++f;
    auto and_expression = &m_expression.And(compile(e1), compile(e2));
    return *std::accumulate(f, l, and_expression, [this](NativeJIT::Node<bool>* and_expr, const ExprNode* expr) {
        return &m_expression.And(*and_expr, compile(expr));
    });
}

NativeJIT::Node<bool>& CompiledFilter::compile_or(const OrExpr* e) {
    assert(e->children().size() >= 2);
    auto f = e->children().begin();
    auto l = e->children().end();
    const auto* e1 = *f;
    ++f;
    const auto* e2 = *f;
    ++f;
    auto or_expression = &m_expression.Or(compile(e1), compile(e2));
    return *std::accumulate(f, l, or_expression, [this](NativeJIT::Node<bool>* or_expr, const ExprNode* expr) {
        return &m_expression.Or(*or_expr, compile(expr));
    });
}

NativeJIT::Node<bool>& CompiledFilter::compile_not(const NotExpr* e) {
    return m_expression.If(compile(e->child()),
                           m_expression.Immediate(false),
                           m_expression.Immediate(true)
    );
}

NativeJIT::Node<bool>& CompiledFilter::check_object_type(const CheckObjectTypeExpr* e) {
    auto& func = m_expression.Immediate(detail::get_type);
    auto& call = m_expression.Call(func, m_expression.GetP1());
    auto& compare = m_expression.Compare<NativeJIT::JccType::JE>(call, m_expression.Immediate(int(e->type())));
    return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
}

NativeJIT::Node<bool>& CompiledFilter::check_has_key(const CheckHasKeyExpr* e) {
    auto& func = m_expression.Immediate(detail::has_key);

    return m_expression.Call(func,
        m_expression.GetP1(),
        m_expression.Immediate(e->key())
    );
}

NativeJIT::Node<bool>& CompiledFilter::check_tag_str(const CheckTagStrExpr* e) {
    auto& func = m_expression.Immediate(
        e->oper()[0] == '!' ? detail::check_tag_not_equals
                            : detail::check_tag_equals
    );

    return m_expression.Call(func,
        m_expression.GetP1(),
        m_expression.Immediate(e->key()),
        m_expression.Immediate(e->value())
    );
}

NativeJIT::Node<bool>& CompiledFilter::check_tag_regex(const CheckTagRegexExpr* e) {
    auto& func = m_expression.Immediate(
        e->oper()[0] == '!' ? detail::check_tag_not_match
                            : detail::check_tag_match
    );

    return m_expression.Call(func,
        m_expression.GetP1(),
        m_expression.Immediate(e->key()),
        m_expression.Immediate(e->value_regex())
    );
}

NativeJIT::Node<bool>& CompiledFilter::check_attr_int(const CheckAttrIntExpr* e) {
    auto& func = m_expression.Immediate(
        e->attr() == "@id"        ? detail::get_id :
        e->attr() == "@uid"       ? detail::get_uid :
        e->attr() == "@changeset" ? detail::get_changeset :
        e->attr() == "@version"   ? detail::get_version :
        e->attr() == "@nodes"     ? detail::get_count_nodes :
        e->attr() == "@members"   ? detail::get_count_members :
        e->attr() == "@tags"      ? detail::get_count_tags :
                                    0
    );

    auto& call = m_expression.Call(func, m_expression.GetP1());

    auto oper = e->oper();
    auto value = e->value();
    if (oper == "=") {
        auto& compare = m_expression.Compare<NativeJIT::JccType::JE>(call, m_expression.Immediate(value));
        return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
    } else if (oper == "!=") {
        auto& compare = m_expression.Compare<NativeJIT::JccType::JNE>(call, m_expression.Immediate(value));
        return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
    } else if (oper == ">") {
        auto& compare = m_expression.Compare<NativeJIT::JccType::JG>(call, m_expression.Immediate(value));
        return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
    } else if (oper == ">=") {
        auto& compare = m_expression.Compare<NativeJIT::JccType::JGE>(call, m_expression.Immediate(value));
        return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
    } else if (oper == "<") {
        auto& compare = m_expression.Compare<NativeJIT::JccType::JL>(call, m_expression.Immediate(value));
        return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
    } else if (oper == "<=") {
        auto& compare = m_expression.Compare<NativeJIT::JccType::JLE>(call, m_expression.Immediate(value));
        return m_expression.Conditional(compare, m_expression.Immediate(true), m_expression.Immediate(false));
    } else {
        assert(false);
    }
}

NativeJIT::Node<bool>& CompiledFilter::compile(const ExprNode* node) {
    switch (node->expression_type()) {
        case expr_node_type::and_expr:
            return compile_and(static_cast<const AndExpr*>(node));
        case expr_node_type::or_expr:
            return compile_or(static_cast<const OrExpr*>(node));
        case expr_node_type::not_expr:
            return compile_not(static_cast<const NotExpr*>(node));
        case expr_node_type::check_has_type:
            return check_object_type(static_cast<const CheckObjectTypeExpr*>(node));
        case expr_node_type::check_has_key:
            return check_has_key(static_cast<const CheckHasKeyExpr*>(node));
        case expr_node_type::check_tag_str:
            return check_tag_str(static_cast<const CheckTagStrExpr*>(node));
        case expr_node_type::check_tag_regex:
            return check_tag_regex(static_cast<const CheckTagRegexExpr*>(node));
//        case expr_node_type::check_attr_int:
        default:
            break;
    }
    return check_attr_int(static_cast<const CheckAttrIntExpr*>(node));
}
