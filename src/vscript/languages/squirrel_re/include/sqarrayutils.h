#ifndef _SQARRAYUTILS_H_
#define _SQARRAYUTILS_H_

//-----------------------------------------------------------------------------
// Utility to convert squirrel arrays to native counterpart.
//-----------------------------------------------------------------------------

#include "squirrel.h"
#include "sqobject.h"
#include "sqstring.h"
#include "sqarray.h"
#include <vector>
#include <string>
#include <type_traits>

template <typename T>
bool ConvertSQObjectPtr(const SQObjectPtr& obj, T& outVal)
{
	if constexpr (std::is_same_v<T, int>)
	{
		if (sq_type(obj) == OT_INTEGER)
		{
			outVal = _integer(obj);
			return true;
		}
		return false;
	}
	else if constexpr (std::is_same_v<T, float>)
	{
		if (sq_type(obj) == OT_FLOAT)
		{
			outVal = _float(obj);
			return true;
		}
		return false;
	}
	else if constexpr (std::is_same_v<T, bool>)
	{
		if (sq_type(obj) == OT_BOOL) {
			outVal = _bool(obj);
			return true;
		}
		return false;
	}
	else if constexpr (std::is_same_v<T, std::string>)
	{
		if (sq_type(obj) == OT_STRING) {
			const SQChar* str = _stringval(obj);
			if (str) {
				outVal = str;
				return true;
			}
		}
		return false;
	}
	else if constexpr (std::is_same_v<T, const char*>)
	{
		if (sq_type(obj) == OT_STRING)
		{
			outVal = _stringval(obj);
			return (outVal != nullptr);
		}
		return false;
	}
	else
	{
		bool check = std::is_same_v<T, void>;
		Assert( check, "Conversion for this type is not supported");
		return false;
	}
}

//------------------------------------------------------------------------------
// Templated function to convert a Squirrel array to a std::vector<T>
//
// Supported template parameters T:
//   - int            => SQArrayToVector<int>(v, idx)
//   - float          => SQArrayToVector<float>(v, idx)
//   - bool           => SQArrayToVector<bool>(v, idx)
//   - std::string    => SQArrayToVector<std::string>(v, idx)
//   - const char*	  => SQArrayToVector<const char*>(v, idx)
//
//------------------------------------------------------------------------------
template <typename T>
std::vector<T> SQArrayToVector(HSQUIRRELVM v, SQInteger idx)
{
	std::vector<T> result;
	idx = sq_absindex(v, idx);

	if (sq_gettype(v, idx) != OT_ARRAY)
	{
		v_SQVM_ScriptError("param at idx %d is not an array.\n", idx);
		return result;
	}

	SQInteger arrSize = 0;
	if (SQ_FAILED(sq_getarraysize(v, idx, &arrSize)))
	{
		v_SQVM_ScriptError("invalid array at idx %d.\n", idx);
		return result;
	}

	if (arrSize == 0)
		return result;

	result.reserve(arrSize);

	SQObject arrObj = stack_get(v, idx);
	SQArray* arr = _array(arrObj);

	if (!arr)
	{
		v_SQVM_ScriptError("array is null\n");
		return result;
	}

	SQObjectPtr firstElement;
	if (!arr->Get(0, firstElement))
	{
		v_SQVM_ScriptError("failed to get first element.\n");
		return result;
	}
	SQObjectType expectedType = sq_type(firstElement);

	for (SQInteger i = 0; i < arrSize; ++i)
	{
		SQObjectPtr element;
		if (!arr->Get(i, element))
		{
			v_SQVM_ScriptError("failed to get element at index %d\n", (int)i);
			continue;
		}

		T val{};
		if (!ConvertSQObjectPtr(element, val))
		{
			v_SQVM_ScriptError
			(
				"type mismatch at index %d: expected array<%s>, got array<%s>\n",
				(int)i, IdType2Name(expectedType),
				IdType2Name(sq_type(element))
			);

			continue;
		}

		result.push_back(val);
	}

	return result;
}
#endif
