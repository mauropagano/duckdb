#include "duckdb_python/pyrelation.hpp"
#include "duckdb_python/pyconnection.hpp"
#include "duckdb_python/pyresult.hpp"
#include "duckdb_python/pandas_analyzer.hpp"
#include "duckdb_python/python_conversion.hpp"
#include "duckdb/common/types/decimal.hpp"

namespace duckdb {

static bool TypeIsNested(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return true;
	default:
		return false;
	}
}

static bool UpgradeType(LogicalType &left, const LogicalType &right);

static bool SameTypeRealm(LogicalTypeId a, LogicalTypeId b) {
	if (a == b) {
		return true;
	}
	if (a > b) {
		return SameTypeRealm(b, a);
	}
	D_ASSERT(a < b);

	// anything ANY and under can transform to anything
	if (a <= LogicalTypeId::ANY) {
		return true;
	}
	auto a_is_nested = TypeIsNested(a);
	auto b_is_nested = TypeIsNested(b);
	// Both a and b are not nested
	if (!a_is_nested && !b_is_nested) {
		return true;
	}
	// Non-nested -> Nested is not possible
	if (!a_is_nested || !b_is_nested) {
		return false;
	}
	// STRUCT -> LIST is not possible
	if (b == LogicalTypeId::LIST || a == LogicalTypeId::LIST) {
		return false;
	}
	return true;
}

//@return Whether the two logicaltypes are compatible
static bool CheckTypeCompatibility(const LogicalType &left, const LogicalType &right) {
	return SameTypeRealm(left.id(), right.id());
}

static bool IsStructColumnValid(const LogicalType &left, const LogicalType &right) {
	D_ASSERT(left.id() == LogicalTypeId::STRUCT && left.id() == right.id());

	//! Child types of the two structs
	auto &left_children = StructType::GetChildTypes(left);
	auto &right_children = StructType::GetChildTypes(right);

	if (left_children.size() != right_children.size()) {
		return false;
	}
	//! Compare keys of struct case-insensitively
	auto compare = CaseInsensitiveStringEquality();
	for (idx_t i = 0; i < left_children.size(); i++) {
		auto &left_child = left_children[i];
		auto &right_child = right_children[i];

		// keys in left and right don't match
		if (!compare(left_child.first, right_child.first)) {
			return false;
		}
		// Types are not compatible with each other
		if (!CheckTypeCompatibility(left_child.second, right_child.second)) {
			return false;
		}
	}
	return true;
}

static bool SatisfiesMapConstraints(const LogicalType &left, const LogicalType &right, LogicalType &map_value_type) {
	D_ASSERT(left.id() == LogicalTypeId::STRUCT && left.id() == right.id());

	//! Child types of the two structs
	auto &left_children = StructType::GetChildTypes(left);
	auto &right_children = StructType::GetChildTypes(right);

	for (auto &type : left_children) {
		if (!UpgradeType(map_value_type, type.second)) {
			return false;
		}
	}
	for (auto &type : right_children) {
		if (!UpgradeType(map_value_type, type.second)) {
			return false;
		}
	}
	return true;
}

static LogicalType ConvertStructToMap(LogicalType &map_value_type) {
	child_list_t<LogicalType> children;
	// TODO: find a way to figure out actual type of the keys, not just the converted one
	children.push_back(make_pair("key", LogicalType::LIST(LogicalType::VARCHAR)));
	children.push_back(make_pair("value", LogicalType::LIST(map_value_type)));
	return LogicalType::MAP(move(children));
}

static bool UpgradeType(LogicalType &left, const LogicalType &right) {
	bool compatible = CheckTypeCompatibility(left, right);
	if (!compatible) {
		return false;
	}
	// If struct constraints are not respected, left will be set to MAP
	if (left.id() == LogicalTypeId::STRUCT && right.id() == left.id()) {
		if (!IsStructColumnValid(left, right)) {
			LogicalType map_value_type = LogicalType::SQLNULL;
			if (SatisfiesMapConstraints(left, right, map_value_type)) {
				left = ConvertStructToMap(map_value_type);
			} else {
				return false;
			}
		}
	}
	// If one of the types is map, this will set the resulting type to map
	left = LogicalType::MaxLogicalType(left, right);
	return true;
}

LogicalType PandasAnalyzer::GetListType(py::handle &ele, bool &can_convert) {
	auto size = py::len(ele);

	if (size == 0) {
		return LogicalType::LIST(LogicalType::SQLNULL);
	}

	idx_t i = 0;
	LogicalType list_type = LogicalType::SQLNULL;
	for (auto py_val : ele) {
		auto item_type = GetItemType(py_val, can_convert);
		if (!i) {
			list_type = item_type;
		} else {
			if (!UpgradeType(list_type, item_type)) {
				can_convert = false;
			}
		}
		if (!can_convert) {
			break;
		}
		i++;
	}
	return LogicalType::LIST(list_type);
}

static LogicalType EmptyMap() {
	child_list_t<LogicalType> child_types;
	auto empty = LogicalType::LIST(LogicalTypeId::SQLNULL);
	child_types.push_back(make_pair("key", empty));
	child_types.push_back(make_pair("value", empty));
	return LogicalType::MAP(move(child_types));
}

//! Check if the keys match
static bool StructKeysAreEqual(idx_t row, const child_list_t<LogicalType> &reference,
                               const child_list_t<LogicalType> &compare) {
	D_ASSERT(reference.size() == compare.size());
	for (idx_t i = 0; i < reference.size(); i++) {
		auto &ref = reference[i].first;
		auto &comp = compare[i].first;
		if (!duckdb::CaseInsensitiveStringEquality()(ref, comp)) {
			return false;
		}
	}
	return true;
}

// Verify that all struct entries in a column have the same amount of fields and that keys are equal
static bool VerifyStructValidity(vector<LogicalType> &structs) {
	D_ASSERT(!structs.empty());
	idx_t reference_entry = 0;
	// Get first non-null entry
	for (; reference_entry < structs.size(); reference_entry++) {
		if (structs[reference_entry].id() != LogicalTypeId::SQLNULL) {
			break;
		}
	}
	// All entries are NULL
	if (reference_entry == structs.size()) {
		return true;
	}
	auto reference_type = structs[reference_entry];
	auto reference_children = StructType::GetChildTypes(reference_type);

	for (idx_t i = reference_entry + 1; i < structs.size(); i++) {
		auto &entry = structs[i];
		if (entry.id() == LogicalTypeId::SQLNULL) {
			continue;
		}
		auto &entry_children = StructType::GetChildTypes(entry);
		if (entry_children.size() != reference_children.size()) {
			return false;
		}
		if (!StructKeysAreEqual(i, reference_children, entry_children)) {
			return false;
		}
	}
	return true;
}

LogicalType PandasAnalyzer::DictToMap(const PyDictionary &dict, bool &can_convert) {
	auto keys = dict.values.attr("__getitem__")(0);
	auto values = dict.values.attr("__getitem__")(1);

	child_list_t<LogicalType> child_types;
	auto key_type = GetListType(keys, can_convert);
	if (!can_convert) {
		return EmptyMap();
	}
	auto value_type = GetListType(values, can_convert);
	if (!can_convert) {
		return EmptyMap();
	}

	child_types.push_back(make_pair("key", key_type));
	child_types.push_back(make_pair("value", value_type));
	return LogicalType::MAP(move(child_types));
}

//! Python dictionaries don't allow duplicate keys, so we don't need to check this.
LogicalType PandasAnalyzer::DictToStruct(const PyDictionary &dict, bool &can_convert) {
	child_list_t<LogicalType> struct_children;

	for (idx_t i = 0; i < dict.len; i++) {
		auto dict_key = dict.keys.attr("__getitem__")(i);

		//! Have to already transform here because the child_list needs a string as key
		auto key = string(py::str(dict_key));

		auto dict_val = dict.values.attr("__getitem__")(i);
		auto val = GetItemType(dict_val, can_convert);
		struct_children.push_back(make_pair(key, move(val)));
	}
	return LogicalType::STRUCT(move(struct_children));
}

//! 'can_convert' is used to communicate if internal structures encountered here are valid
//! e.g python lists can consist of multiple different types, which we cant communicate downwards through
//! LogicalType's alone

LogicalType PandasAnalyzer::GetItemType(py::handle ele, bool &can_convert) {
	auto &import_cache = *DuckDBPyConnection::ImportCache();

	if (ele.is_none()) {
		return LogicalType::SQLNULL;
	} else if (py::isinstance<py::bool_>(ele)) {
		return LogicalType::BOOLEAN;
	} else if (py::isinstance<py::int_>(ele)) {
		Value integer;
		if (!TryTransformPythonInteger(integer, ele)) {
			can_convert = false;
			return LogicalType::SQLNULL;
		}
		return integer.type();
	} else if (py::isinstance<py::float_>(ele)) {
		if (std::isnan(PyFloat_AsDouble(ele.ptr()))) {
			return LogicalType::SQLNULL;
		}
		return LogicalType::DOUBLE;
	} else if (py::isinstance(ele, import_cache.decimal.Decimal())) {
		PyDecimal decimal(ele);
		LogicalType type;
		if (!decimal.TryGetType(type)) {
			can_convert = false;
		}
		return type;
	} else if (py::isinstance(ele, import_cache.datetime.datetime())) {
		return LogicalType::TIMESTAMP;
	} else if (py::isinstance(ele, import_cache.datetime.time())) {
		return LogicalType::TIME;
	} else if (py::isinstance(ele, import_cache.datetime.date())) {
		return LogicalType::DATE;
	} else if (py::isinstance(ele, import_cache.datetime.timedelta())) {
		return LogicalType::INTERVAL;
	} else if (py::isinstance<py::str>(ele)) {
		return LogicalType::VARCHAR;
	} else if (py::isinstance(ele, import_cache.uuid.UUID())) {
		return LogicalType::UUID;
	} else if (py::isinstance<py::bytearray>(ele)) {
		return LogicalType::BLOB;
	} else if (py::isinstance<py::memoryview>(ele)) {
		return LogicalType::BLOB;
	} else if (py::isinstance<py::bytes>(ele)) {
		return LogicalType::BLOB;
	} else if (py::isinstance<py::list>(ele)) {
		return GetListType(ele, can_convert);
	} else if (py::isinstance<py::dict>(ele)) {
		PyDictionary dict = PyDictionary(py::reinterpret_borrow<py::object>(ele));
		// Assuming keys and values are the same size

		if (dict.len == 0) {
			return EmptyMap();
		}
		if (DictionaryHasMapFormat(dict)) {
			return DictToMap(dict, can_convert);
		}
		return DictToStruct(dict, can_convert);
	} else if (py::isinstance(ele, import_cache.numpy.ndarray())) {
		auto dtype_str = string(py::str(ele.attr("dtype")));
		auto extended_type = ConvertPandasType(dtype_str);
		LogicalType ltype;
		ltype = PandasToLogicalType(extended_type);
		if (extended_type == PandasType::OBJECT) {
			LogicalType converted_type = InnerAnalyze(ele, can_convert, false, 1);
			if (can_convert) {
				ltype = converted_type;
			}
		}
		return LogicalType::LIST(ltype);
	} else {
		// Fall back to string for unknown types
		can_convert = false;
		return LogicalType::VARCHAR;
	}
}

//! Get the increment for the given sample size
uint64_t PandasAnalyzer::GetSampleIncrement(idx_t rows) {
	D_ASSERT(sample_size != 0);
	//! Apply the maximum
	auto sample = sample_size;
	if (sample > rows) {
		sample = rows;
	}
	return rows / sample;
}

LogicalType PandasAnalyzer::InnerAnalyze(py::handle column, bool &can_convert, bool sample, idx_t increment) {
	idx_t rows = py::len(column);

	if (!rows) {
		return LogicalType::SQLNULL;
	}

	// Keys are not guaranteed to start at 0 for Series, use the internal __array__ instead
	auto pandas_module = py::module::import("pandas");
	auto pandas_series = pandas_module.attr("core").attr("series").attr("Series");
	if (py::isinstance(column, pandas_series)) {
		column = column.attr("__array__")();
	}
	auto row = column.attr("__getitem__");

	vector<LogicalType> types;
	auto item_type = GetItemType(row(0), can_convert);
	if (!can_convert) {
		return item_type;
	}
	types.push_back(item_type);

	if (sample) {
		increment = GetSampleIncrement(rows);
	}
	for (idx_t i = increment; i < rows; i += increment) {
		auto next_item_type = GetItemType(row(i), can_convert);
		types.push_back(next_item_type);

		if (!can_convert || !UpgradeType(item_type, next_item_type)) {
			can_convert = false;
			return next_item_type;
		}
	}

	if (can_convert && item_type.id() == LogicalTypeId::STRUCT) {
		can_convert = VerifyStructValidity(types);
	}

	return item_type;
}

bool PandasAnalyzer::Analyze(py::handle column) {
	// Disable analyze
	if (sample_size == 0) {
		return false;
	}
	bool can_convert = true;
	LogicalType type = InnerAnalyze(column, can_convert);
	if (can_convert) {
		analyzed_type = type;
	}
	return can_convert;
}

} // namespace duckdb
