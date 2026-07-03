/*
 * Copyright (c) 2005 Palmsource, Inc.
 * 
 * This software is licensed as described in the file LICENSE, which
 * you should have received as part of this distribution. The terms
 * are also available at http://www.openbinder.org/license.html.
 * 
 * This software consists of voluntary contributions made by many
 * individuals. For the exact contribution history, see the revision
 * history and logs, available at http://www.openbinder.org
 */

#ifndef _SUPPORT_TYPECONSTANTS_H
#define _SUPPORT_TYPECONSTANTS_H

/*!	@file support/TypeConstants.h
	@ingroup CoreSupportUtilities
	@brief Format and standard definitions of SValue type codes.
*/

#ifdef __cplusplus
#if _SUPPORTS_NAMESPACE
namespace openbinder {
namespace support {
#endif
#endif

/*!	@addtogroup CoreSupportUtilities
	@{
*/

/*-------------------------------------------------------------*/
/*----- Data Types --------------------------------------------*/

/*!	@name Type Code Definitions
	Type codes are 32-bit integers.  The upper 24 bits are the
	the code, and the lower 8 bits are metadata.  The code is
	constructed as 3 characters.  Codes containing only the
	characters a-z and _, and codes whose last letter is not
	alphabetic (a-zA-Z), are reserved for use by the system.
	Type codes that end with the character '*' contain
	pointers to external objects.
	Type codes that end with the character '#' are in a special
	namespace reserved for SDimth units. */
//@{

//!	Type code manipulation.
enum {
	B_TYPE_CODE_MASK			= 0x7f7f7f00,	// Usable bits for the type code
	B_TYPE_CODE_SHIFT			= 8,			// Where code appears.

	B_TYPE_LENGTH_MASK			= 0x00000007,	// Usable bits for the data length
	B_TYPE_LENGTH_MAX			= 0x00000004,	// Largest length that can be encoded in type
	B_TYPE_LENGTH_LARGE			= 0x00000005,	// Value when length is > 4 bytes
	B_TYPE_LENGTH_MAP			= 0x00000007,	// For use by SValue

	B_TYPE_BYTEORDER_MASK		= 0x80000080,	// Bits used to check byte order
	B_TYPE_BYTEORDER_NORMAL		= 0x00000080,	// This bit is set if the byte order is okay
	B_TYPE_BYTEORDER_SWAPPED	= 0x80000000	// This bit is set if the byte order is swapped
};

//! Pack a small (size <= B_TYPE_LENGTH_MAX) type code from its constituent parts.
#define B_PACK_SMALL_TYPE(code, length)			(((code)&B_TYPE_CODE_MASK) | (length) | B_TYPE_BYTEORDER_NORMAL)
//! Pack a large (size > B_TYPE_LENGTH_MAX) type code from its constituent parts.
#define B_PACK_LARGE_TYPE(code)					(((code)&B_TYPE_CODE_MASK) | B_TYPE_LENGTH_LARGE | B_TYPE_BYTEORDER_NORMAL)
//!	Retrieve type information from a packed type code.
#define B_UNPACK_TYPE_CODE(type)				((type)&B_TYPE_CODE_MASK)
//!	Retrieve size information from a packaed type code.
#define B_UNPACK_TYPE_LENGTH(type)				((type)&B_TYPE_LENGTH_MASK)

//!	Build a valid code for a type code.
/*!	Ensures only correct bits are used, and shifts value into correct location. */
#define B_TYPE_CODE(code)						(((code)<<B_TYPE_CODE_SHIFT)&B_TYPE_CODE_MASK)

#define B_PACK_CHARS(c1, c2, c3, c4) ((((c1)<<24)) | (((c2)<<16)) | (((c3)<<8)) | (c4))

//!	Standard type codes.
enum {
	// Special code to match any type code in comparisons.
	B_ANY_TYPE 					= B_TYPE_CODE(B_PACK_CHARS(0, 'a','n','y')),
	
	// Object types.
	B_BINDER_TYPE				= B_TYPE_CODE(B_PACK_CHARS(0, 's','b','*')),
	B_BINDER_WEAK_TYPE			= B_TYPE_CODE(B_PACK_CHARS(0, 'w','b','*')),
	B_BINDER_HANDLE_TYPE		= B_TYPE_CODE(B_PACK_CHARS(0, 's','h','*')),
	B_BINDER_WEAK_HANDLE_TYPE	= B_TYPE_CODE(B_PACK_CHARS(0, 'w','h','*')),
	B_BINDER_NODE_TYPE			= B_TYPE_CODE(B_PACK_CHARS(0, 's','n','*')),
	B_BINDER_WEAK_NODE_TYPE		= B_TYPE_CODE(B_PACK_CHARS(0, 'w','n','*')),
	B_ATOM_TYPE 				= B_TYPE_CODE(B_PACK_CHARS(0, 's','a','*')),
	B_ATOM_WEAK_TYPE 			= B_TYPE_CODE(B_PACK_CHARS(0, 'w','a','*')),
	B_FILE_DESCRIPTOR_TYPE 		= B_TYPE_CODE(B_PACK_CHARS(0, 'f','d','*'))
};

/*-------------------------------------------------------------*/

/*!	@} */

#ifdef __cplusplus
#if _SUPPORTS_NAMESPACE
} }	// namespace openbinder::support
#endif

#endif

#endif /* _SUPPORT_TYPECONSTANTS_H */
