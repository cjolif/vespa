// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "attributedfw.h"
#include "docsumstate.h"
#include "docsumwriter.h"
#include "docsum_field_writer_state.h"
#include <vespa/eval/eval/value.h>
#include <vespa/eval/eval/value_codec.h>
#include <vespa/searchcommon/attribute/i_multi_value_attribute.h>
#include <vespa/searchcommon/attribute/multi_value_traits.h>
#include <vespa/searchlib/attribute/iattributemanager.h>
#include <vespa/searchlib/common/matching_elements.h>
#include <vespa/searchlib/common/matching_elements_fields.h>
#include <vespa/searchlib/tensor/i_tensor_attribute.h>
#include <vespa/vespalib/data/slime/slime.h>
#include <vespa/vespalib/objects/nbostream.h>
#include <vespa/vespalib/util/issue.h>

#include <vespa/log/log.h>
LOG_SETUP(".searchlib.docsummary.attributedfw");

using namespace search;
using search::attribute::BasicType;
using search::attribute::IAttributeContext;
using search::attribute::IAttributeVector;
using search::attribute::IMultiValueAttribute;
using search::attribute::IMultiValueReadView;
using vespalib::Memory;
using vespalib::slime::Cursor;
using vespalib::slime::Inserter;
using vespalib::slime::Symbol;
using vespalib::eval::Value;
using vespalib::Issue;

namespace search::docsummary {

AttrDFW::AttrDFW(const vespalib::string & attrName) :
    _attrName(attrName)
{
}

const attribute::IAttributeVector &
AttrDFW::get_attribute(const GetDocsumsState& s) const {
    return *s.getAttribute(getIndex());
}

namespace {

class SingleAttrDFW : public AttrDFW
{
public:
    explicit SingleAttrDFW(const vespalib::string & attrName) :
        AttrDFW(attrName)
    { }
    void insertField(uint32_t docid, GetDocsumsState *state, ResType type, Inserter &target) override;
    bool isDefaultValue(uint32_t docid, const GetDocsumsState * state) const override;
};

bool SingleAttrDFW::isDefaultValue(uint32_t docid, const GetDocsumsState * state) const
{
    return get_attribute(*state).isUndefined(docid);
}

void
SingleAttrDFW::insertField(uint32_t docid, GetDocsumsState * state, ResType type, Inserter &target)
{
    const auto& v = get_attribute(*state);
    switch (type) {
    case RES_INT: {
        uint32_t val = v.getInt(docid);
        target.insertLong(val);
        break;
    }
    case RES_SHORT: {
        uint16_t val = v.getInt(docid);
        target.insertLong(val);
        break;
    }
    case RES_BYTE: {
        uint8_t val = v.getInt(docid);
        target.insertLong(val);
        break;
    }
    case RES_BOOL: {
        uint8_t val = v.getInt(docid);
        target.insertBool(val != 0);
        break;
    }
    case RES_FLOAT: {
        float val = v.getFloat(docid);
        target.insertDouble(val);
        break;
    }
    case RES_DOUBLE: {
        double val = v.getFloat(docid);
        target.insertDouble(val);
        break;
    }
    case RES_INT64: {
        uint64_t val = v.getInt(docid);
        target.insertLong(val);
        break;
    }
    case RES_TENSOR: {
        BasicType::Type t = v.getBasicType();
        switch (t) {
        case BasicType::TENSOR: {
            const tensor::ITensorAttribute *tv = v.asTensorAttribute();
            assert(tv != nullptr);
            const auto tensor = tv->getTensor(docid);
            if (tensor) {
                vespalib::nbostream str;
                encode_value(*tensor, str);
                target.insertData(vespalib::Memory(str.peek(), str.size()));
            }
        }
        default:
            ;
        }
    }
        break;
    case RES_JSONSTRING:
    case RES_FEATUREDATA:
    case RES_LONG_STRING:
    case RES_STRING: {
        const char *s = v.getString(docid, nullptr, 0); // no need to pass in a buffer, this attribute has a string storage.
        target.insertString(vespalib::Memory(s));
        break;
    }
    case RES_LONG_DATA:
    case RES_DATA: {
        const char *s = v.getString(docid, nullptr, 0); // no need to pass in a buffer, this attribute has a string storage.
        target.insertData(vespalib::Memory(s));
        break;
    }
    default:
        // unknown type, will be missing, should not happen
        return;
    }
}


//-----------------------------------------------------------------------------

template <typename MultiValueType>
const IMultiValueReadView<MultiValueType>*
make_read_view(const IAttributeVector& attribute, vespalib::Stash& stash)
{
    auto multi_value_attribute = attribute.as_multi_value_attribute();
    if (multi_value_attribute != nullptr) {
        return multi_value_attribute->make_read_view(IMultiValueAttribute::MultiValueTag<MultiValueType>(), stash);
    }
    return nullptr;
}

class EmptyWriterState : public DocsumFieldWriterState
{
public:
    EmptyWriterState() = default;
    ~EmptyWriterState() = default;
    void insertField(uint32_t, Inserter&) override { }
};

template <typename MultiValueType>
class MultiAttrDFWState : public DocsumFieldWriterState
{
    const vespalib::string&                    _field_name;
    const IMultiValueReadView<MultiValueType>* _read_view;
    const MatchingElements*                    _matching_elements;
public:
    MultiAttrDFWState(const vespalib::string& field_name, const IAttributeVector& attr, vespalib::Stash& stash, const MatchingElements* matching_elements);
    ~MultiAttrDFWState() override;
    void insertField(uint32_t docid, Inserter& target) override;
};


template <typename MultiValueType>
MultiAttrDFWState<MultiValueType>::MultiAttrDFWState(const vespalib::string& field_name, const IAttributeVector& attr, vespalib::Stash& stash, const MatchingElements* matching_elements)
    : _field_name(field_name),
      _read_view(make_read_view<MultiValueType>(attr, stash)),
      _matching_elements(matching_elements)
{
}

template <typename MultiValueType>
MultiAttrDFWState<MultiValueType>::~MultiAttrDFWState() = default;

template <typename V>
void
set_value(V value, Symbol item_symbol, Cursor& cursor)
{
    if constexpr (std::is_same_v<V, const char*>) {
        cursor.setString(item_symbol, value);
    } else if constexpr(std::is_floating_point_v<V>) {
        cursor.setDouble(item_symbol, value);
    } else {
        cursor.setLong(item_symbol, value);
    }
}

template <typename V>
void
append_value(V value, Cursor& arr)
{
    if constexpr (std::is_same_v<V, const char*>) {
        arr.addString(value);
    } else if constexpr(std::is_floating_point_v<V>) {
        arr.addDouble(value);
    } else {
        arr.addLong(value);
    }
}

Memory ITEM("item");
Memory WEIGHT("weight");

template <typename MultiValueType>
void
MultiAttrDFWState<MultiValueType>::insertField(uint32_t docid, Inserter& target)
{
    using ValueType = multivalue::ValueType_t<MultiValueType>;
    if (!_read_view) {
        return;
    }
    auto elements = _read_view->get_values(docid);
    if (elements.empty()) {
        return;
    }
    Cursor &arr = target.insertArray(elements.size());

    if (_matching_elements) {
        const auto& matching_elems = _matching_elements->get_matching_elements(docid, _field_name);
        if (!matching_elems.empty() && matching_elems.back() < elements.size()) {
            if constexpr (multivalue::is_WeightedValue_v<MultiValueType>) {
                Symbol itemSymbol = arr.resolve(ITEM);
                Symbol weightSymbol = arr.resolve(WEIGHT);
                for (uint32_t id_to_keep : matching_elems) {
                    auto& element = elements[id_to_keep];
                    Cursor& elemC = arr.addObject();
                    set_value<ValueType>(element.value(), itemSymbol, elemC);
                    elemC.setLong(weightSymbol, element.weight());
                }
            } else {
                for (uint32_t id_to_keep : matching_elems) {
                    append_value<ValueType>(elements[id_to_keep], arr);
                }
            }
        }
    } else {
        if constexpr (multivalue::is_WeightedValue_v<MultiValueType>) {
            Symbol itemSymbol = arr.resolve(ITEM);
            Symbol weightSymbol = arr.resolve(WEIGHT);
            for (const auto & element : elements) {
                Cursor& elemC = arr.addObject();
                set_value<ValueType>(element.value(), itemSymbol, elemC);
                elemC.setLong(weightSymbol, element.weight());
            }
        } else {
            for (const auto & element : elements) {
                append_value<ValueType>(element, arr);
            }
        }
    }
}

class MultiAttrDFW : public AttrDFW {
private:
    bool _filter_elements;
    uint32_t _state_index; // index into _fieldWriterStates in GetDocsumsState
    std::shared_ptr<MatchingElementsFields> _matching_elems_fields;

public:
    explicit MultiAttrDFW(const vespalib::string& attr_name, bool filter_elements, std::shared_ptr<MatchingElementsFields> matching_elems_fields)
        : AttrDFW(attr_name),
          _filter_elements(filter_elements),
          _matching_elems_fields(std::move(matching_elems_fields))
    {
        if (filter_elements && _matching_elems_fields) {
            _matching_elems_fields->add_field(attr_name);
        }
    }
    bool setFieldWriterStateIndex(uint32_t fieldWriterStateIndex) override;
    void insertField(uint32_t docid, GetDocsumsState* state, ResType type, Inserter& target) override;
};

bool
MultiAttrDFW::setFieldWriterStateIndex(uint32_t fieldWriterStateIndex)
{
    _state_index = fieldWriterStateIndex;
    return true;
}

template <typename DataType>
DocsumFieldWriterState*
make_field_writer_state_helper(const vespalib::string& field_name, const IAttributeVector& attr, vespalib::Stash& stash, const MatchingElements* matching_elements)
{
    bool is_weighted_set = attr.hasWeightedSetType();
    if (is_weighted_set) {
        return &stash.create<MultiAttrDFWState<multivalue::WeightedValue<DataType>>>(field_name, attr, stash, matching_elements);
    } else {
        return &stash.create<MultiAttrDFWState<DataType>>(field_name, attr, stash, matching_elements);
    }
}

DocsumFieldWriterState*
make_field_writer_state(const vespalib::string& field_name, const IAttributeVector& attr, vespalib::Stash& stash, const MatchingElements* matching_elements)
{
    auto type = attr.getBasicType();
    switch (type) {
    case BasicType::Type::STRING:
        return make_field_writer_state_helper<const char*>(field_name, attr, stash, matching_elements);
    case BasicType::Type::INT8:
        return make_field_writer_state_helper<int8_t>(field_name, attr, stash, matching_elements);
    case BasicType::Type::INT16:
        return make_field_writer_state_helper<int16_t>(field_name, attr, stash, matching_elements);
    case BasicType::Type::INT32:
        return make_field_writer_state_helper<int32_t>(field_name, attr, stash, matching_elements);
    case BasicType::Type::INT64:
        return make_field_writer_state_helper<int64_t>(field_name, attr, stash, matching_elements);
    case BasicType::Type::FLOAT:
        return make_field_writer_state_helper<float>(field_name, attr, stash, matching_elements);
    case BasicType::Type::DOUBLE:
        return make_field_writer_state_helper<double>(field_name, attr, stash, matching_elements);
    default:
        ;
    }
    return &stash.create<EmptyWriterState>();
}

void
MultiAttrDFW::insertField(uint32_t docid, GetDocsumsState *state, ResType, vespalib::slime::Inserter &target)
{
    auto& field_writer_state = state->_fieldWriterStates[_state_index];
    if (!field_writer_state) {
        const MatchingElements *matching_elements = nullptr;
        if (_filter_elements) {
            matching_elements = &state->get_matching_elements(*_matching_elems_fields);
        }
        const auto& attr = get_attribute(*state);
        field_writer_state = make_field_writer_state(getAttributeName(), attr, state->get_stash(), matching_elements);
    }
    field_writer_state->insertField(docid, target);
}

std::unique_ptr<IDocsumFieldWriter>
create_multi_writer(const IAttributeVector& attr, bool filter_elements, std::shared_ptr<MatchingElementsFields> matching_elems_fields)
{
    auto type = attr.getBasicType();
    switch (type) {
    case BasicType::STRING:
    case BasicType::INT8:
    case BasicType::INT16:
    case BasicType::INT32:
    case BasicType::INT64:
    case BasicType::FLOAT:
    case BasicType::DOUBLE:
        return std::make_unique<MultiAttrDFW>(attr.getName(), filter_elements, std::move(matching_elems_fields));
    default:
        // should not happen
        LOG(error, "Bad value for attribute type: %u", type);
        LOG_ASSERT(false);
    }
}

}

std::unique_ptr<IDocsumFieldWriter>
AttributeDFWFactory::create(IAttributeManager& attr_mgr,
                            const vespalib::string& attr_name,
                            bool filter_elements,
                            std::shared_ptr<MatchingElementsFields> matching_elems_fields)
{
    auto ctx = attr_mgr.createContext();
    const auto* attr = ctx->getAttribute(attr_name);
    if (attr == nullptr) {
        Issue::report("No valid attribute vector found: '%s'", attr_name.c_str());
        return std::unique_ptr<IDocsumFieldWriter>();
    }
    if (attr->hasMultiValue()) {
        return create_multi_writer(*attr, filter_elements, std::move(matching_elems_fields));
    } else {
        return std::make_unique<SingleAttrDFW>(attr->getName());
    }
}

}
