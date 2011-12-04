#include <sasl/include/code_generator/llvm/cgs_sisd.h>

#include <sasl/include/syntax_tree/declaration.h>
#include <sasl/include/semantic/symbol.h>
#include <sasl/include/semantic/semantic_infos.h>
#include <sasl/include/code_generator/llvm/cgllvm_contexts.h>
#include <sasl/enums/enums_utility.h>

#include <eflib/include/platform/disable_warnings.h>
#include <llvm/Support/IRBuilder.h>
#include <llvm/Function.h>
#include <llvm/Module.h>
#include <llvm/Intrinsics.h>
#include <llvm/Support/TypeBuilder.h>
#include <llvm/Support/CFG.h>
#include <eflib/include/platform/enable_warnings.h>

#include <eflib/include/platform/boost_begin.h>
#include <boost/foreach.hpp>
#include <boost/unordered_map.hpp>
#include <boost/lexical_cast.hpp>
#include <eflib/include/platform/boost_end.h>

#include <eflib/include/diagnostics/assert.h>
#include <eflib/include/platform/cpuinfo.h>

using sasl::syntax_tree::node;
using sasl::syntax_tree::function_type;
using sasl::syntax_tree::parameter;
using sasl::syntax_tree::tynode;
using sasl::syntax_tree::declaration;
using sasl::syntax_tree::variable_declaration;
using sasl::syntax_tree::struct_type;

using sasl::semantic::storage_si;
using sasl::semantic::type_info_si;

using eflib::support_feature;
using eflib::cpu_sse2;

using namespace sasl::utility;

using llvm::APInt;
using llvm::Argument;
using llvm::LLVMContext;
using llvm::Function;
using llvm::FunctionType;
using llvm::IntegerType;
using llvm::Type;
using llvm::PointerType;
using llvm::Value;
using llvm::BasicBlock;
using llvm::Constant;
using llvm::ConstantInt;
using llvm::ConstantVector;
using llvm::StructType;
using llvm::VectorType;
using llvm::UndefValue;
using llvm::StoreInst;
using llvm::TypeBuilder;
using llvm::AttrListPtr;
using llvm::SwitchInst;

namespace Intrinsic = llvm::Intrinsic;

using boost::any;
using boost::shared_ptr;
using boost::enable_if;
using boost::is_integral;
using boost::unordered_map;
using boost::lexical_cast;

using std::vector;
using std::string;

// Fn name is function name, op_name is llvm Create##op_name/CreateF##op_name
#define EMIT_OP_SS_VV_BODY( op_name )	\
		builtin_types hint( lhs.get_hint() ); \
		assert( hint == rhs.get_hint() ); \
		assert( is_scalar(hint) || is_vector(hint) ); \
		\
		Value* ret = NULL; \
		\
		builtin_types scalar_hint = is_scalar(hint) ? hint : scalar_of(hint); \
		if( is_real( scalar_hint ) ){ \
			ret = builder().CreateF##op_name ( lhs.load(abi_llvm), rhs.load(abi_llvm) ); \
		} else { \
			ret = builder().Create##op_name( lhs.load(abi_llvm), rhs.load(abi_llvm) ); \
		}	\
		\
		value_t retval( hint, ret, value_t::kind_value, abi_llvm, this ); \
		abis ret_abi = is_scalar(hint) ? abi_llvm : lhs.get_abi();\
		return value_t( hint, retval.load(ret_abi), value_t::kind_value, ret_abi, this );

#define EMIT_CMP_EQ_NE_BODY( op_name ) \
	builtin_types hint = lhs.get_hint(); \
	assert( hint == rhs.get_hint() ); \
	assert( is_scalar(hint) || (hint == builtin_types::_boolean) ); \
	\
	Value* ret = NULL; \
	if( is_integer(hint) || (hint == builtin_types::_boolean) ){ \
		ret = builder().CreateICmp##op_name( lhs.load(), rhs.load() ); \
	} else if( is_real(hint) ) { \
		ret = builder().CreateFCmpU##op_name( lhs.load(), rhs.load() ); \
	} \
	\
	return value_t( builtin_types::_boolean, ret, value_t::kind_value, abi_llvm, this );

#define EMIT_CMP_BODY( op_name ) \
	builtin_types hint = lhs.get_hint(); \
	assert( hint == rhs.get_hint() ); \
	assert( is_scalar(hint) ); \
	\
	Value* ret = NULL; \
	if( is_integer(hint) ){ \
		if( is_signed(hint) ){ \
			ret = builder().CreateICmpS##op_name( lhs.load(), rhs.load() ); \
		} else {\
			ret = builder().CreateICmpU##op_name( lhs.load(), rhs.load() ); \
		}\
		\
	} else if( is_real(hint) ) { \
		ret = builder().CreateFCmpU##op_name( lhs.load(), rhs.load() ); \
	} \
	\
	return value_t( builtin_types::_boolean, ret, value_t::kind_value, abi_llvm, this );

BEGIN_NS_SASL_CODE_GENERATOR();

namespace {
	
	/// Only support a context.
	class ty_cache_t{
	public:
		Type* type( LLVMContext& ctxt, builtin_types bt, abis abi ){
			Type*& found_ty = cache[abi][&ctxt][bt];
			if( !found_ty ){ 
				found_ty = create_ty( ctxt, bt, abi );
			}
			return found_ty;
		}
		std::string const& name( builtin_types bt, abis abi ){
			std::string& ret_name = ty_name[abi][bt];
			if( ret_name.empty() ){
				if( is_scalar(bt) ){
					if( bt == builtin_types::_void ){
						ret_name = "void";
					} else if ( bt == builtin_types::_sint8 ){
						ret_name = "char";
					} else if ( bt == builtin_types::_sint16 ){
						ret_name = "short";
					} else if ( bt == builtin_types::_sint32 ){
						ret_name = "int";
					} else if ( bt == builtin_types::_sint64 ){
						ret_name = "int64";
					} else if ( bt == builtin_types::_uint8 ){
						ret_name = "uchar";
					} else if ( bt == builtin_types::_uint16 ){
						ret_name = "ushort";
					} else if ( bt == builtin_types::_uint32 ){
						ret_name = "uint";
					} else if ( bt == builtin_types::_uint64 ){
						ret_name = "uint64";
					} else if ( bt == builtin_types::_float ){
						ret_name = "float";
					} else if ( bt == builtin_types::_double ){
						ret_name = "double";
					} else if ( bt == builtin_types::_boolean ){
						ret_name = "bool";
					}
				} else if ( is_vector(bt) ){
					ret_name = name( scalar_of(bt), abi );
					ret_name.reserve( ret_name.length() + 5 );
					ret_name += ".v";
					ret_name += lexical_cast<string>( vector_size(bt) );
					ret_name += ( abi == abi_c ? ".c" : ".l" );
				} else if ( is_matrix(bt) ){
					ret_name = name( scalar_of(bt), abi );
					ret_name.reserve( ret_name.length() + 6 );
					ret_name += ".m";
					ret_name += lexical_cast<string>( vector_size(bt) );
					ret_name += lexical_cast<string>( vector_count(bt) );
					ret_name += ( abi == abi_c ? ".c" : ".l" );
				}
			}
			return ret_name;
		}
	private:
		Type* create_ty( LLVMContext& ctxt, builtin_types bt, abis abi ){
			if ( is_void( bt ) ){
				return Type::getVoidTy( ctxt );
			}

			if( is_scalar(bt) ){
				if( bt == builtin_types::_boolean ){
					return IntegerType::get( ctxt, 1 );
				}
				if( is_integer(bt) ){
					return IntegerType::get( ctxt, (unsigned int)storage_size( bt ) << 3 );
				}
				if ( bt == builtin_types::_float ){
					return Type::getFloatTy( ctxt );
				}
				if ( bt == builtin_types::_double ){
					return Type::getDoubleTy( ctxt );
				}
			}

			if( is_vector(bt) ){
				Type* elem_ty = type(ctxt, scalar_of(bt), abi );
				size_t vec_size = vector_size(bt);
				if( abi == abi_c ){
					vector<Type*> elem_tys(vec_size, elem_ty);
					return StructType::create( elem_tys, name(bt, abi) );
				} else {
					return VectorType::get( elem_ty, static_cast<unsigned int>(vec_size) );
				}
			}

			if( is_matrix(bt) ){
				Type* vec_ty = type( ctxt, vector_of( scalar_of(bt), vector_size(bt) ), abi );
				vector<Type*> row_tys( vector_count(bt), vec_ty );
				return StructType::create( row_tys, name(bt, abi) );
			}
		}
		unordered_map<LLVMContext*, unordered_map<builtin_types, Type*> > cache[2];
		unordered_map<builtin_types, std::string> ty_name[2];
	};

	ty_cache_t ty_cache;

	/// Get LLVM type from builtin types.
	Type* get_llvm_type( LLVMContext& ctxt, builtin_types bt, bool is_c_compatible ){
		return ty_cache.type( ctxt, bt, is_c_compatible ? abi_c : abi_llvm );
	}

	template <typename T>
	APInt apint( T v ){
		return APInt( sizeof(v) << 3, static_cast<uint64_t>(v), boost::is_signed<T>::value );
	}

	void mask_to_indexes( char indexes[4], uint32_t mask ){
		for( int i = 0; i < 4; ++i ){
			// XYZW is 1,2,3,4 but LLVM used 0,1,2,3
			char comp_index = static_cast<char>( (mask >> i*8) & 0xFF );
			if( comp_index == 0 ){
				indexes[i] = -1;
				break;
			}
			indexes[i] = comp_index - 1;
		}
	}

	uint32_t indexes_to_mask( char indexes[4] ){
		uint32_t mask = 0;
		for( int i = 0; i < 4; ++i ){
			mask += (uint32_t)( (indexes[i] + 1) << (i*8) );
		}
		return mask;
	}

	uint32_t indexes_to_mask( char idx0, char idx1, char idx2, char idx3 ){
		char indexes[4] = { idx0, idx1, idx2, idx3 };
		return indexes_to_mask( indexes );
	}

	void dbg_print_blocks( Function* fn ){
#ifdef _DEBUG
		/*printf( "Function: 0x%X\n", fn );
		for( Function::BasicBlockListType::iterator it = fn->getBasicBlockList().begin(); it != fn->getBasicBlockList().end(); ++it ){
			printf( "  Block: 0x%X\n", &(*it) );
		}*/
		fn = fn;
#else
		fn = fn;
#endif
	}
}

template <typename T>
ConstantInt* cgs_sisd::int_( T v )
{
	return ConstantInt::get( context(), apint(v) );
}

template <typename T>
llvm::ConstantVector* cgs_sisd::vector_( T const* vals, size_t length, typename enable_if< is_integral<T> >::type* )
{
	assert( vals && length > 0 );
	
	vector<Constant*> elems(length);
	for( size_t i = 0; i < length; ++i ){
		elems[i] = int_(vals[i]);
	}

	return llvm::cast<ConstantVector>( ConstantVector::get( elems ) );
}


template <typename FunctionT>
Function* cgs_sisd::intrin_( int id )
{
	return intrins.get(id, module(), TypeBuilder<FunctionT, false>::get( context() ) );
}

// Value tyinfo
value_tyinfo::value_tyinfo(
	tynode* sty,
	llvm::Type* cty,
	llvm::Type* llty
	) : sty(sty)
{
	llvm_tys[abi_c] = cty;
	llvm_tys[abi_llvm] = llty;
}

value_tyinfo::value_tyinfo()
	:sty(NULL), cls( unknown_type )
{
	llvm_tys[0] = NULL;
	llvm_tys[1] = NULL;
}

builtin_types value_tyinfo::hint() const{
	if( !sty || !sty->is_builtin() ){
		return builtin_types::none;
	}
	return sty->tycode;
}

tynode* value_tyinfo::typtr() const{
	return sty;
}

shared_ptr<tynode> value_tyinfo::tysp() const{
	return sty->as_handle<tynode>();
}

llvm::Type* value_tyinfo::llvm_ty( abis abi ) const
{
	return llvm_tys[abi];
}

/// @}

/// value_t @{
value_t::value_t()
	: tyinfo(NULL), val(NULL), cg(NULL), kind(kind_unknown), hint(builtin_types::none), abi(abi_unknown), masks(0)
{
}

value_t::value_t(
	value_tyinfo* tyinfo,
	llvm::Value* val, value_t::kinds k, abis abi,
	cgs_sisd* cg 
	) 
	: tyinfo(tyinfo), val(val), cg(cg), kind(k), hint(builtin_types::none), abi(abi), masks(0)
{
}

value_t::value_t( builtin_types hint,
	llvm::Value* val, value_t::kinds k, abis abi,
	cgs_sisd* cg 
	)
	: tyinfo(NULL), hint(hint), abi(abi), val(val), kind(k), cg(cg), masks(0)
{

}

value_t::value_t( value_t const& rhs )
	: tyinfo(rhs.tyinfo), hint(rhs.hint), abi(rhs.abi), val( rhs.val ), kind(rhs.kind), cg(rhs.cg), masks(rhs.masks)
{
	set_parent(rhs.parent.get());
}

abis value_t::get_abi() const{
	return abi;
}

value_t value_t::swizzle( size_t swz_code ) const{
	assert( is_vector( get_hint() ) );
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

llvm::Value* value_t::raw() const{
	return val;
}

value_t value_t::to_rvalue() const
{
	if( tyinfo ){
		return value_t( tyinfo, load( abi ), kind_value, abi, cg );
	} else {
		return value_t( hint, load(abi), kind_value, abi, cg );
	}
}

builtin_types value_t::get_hint() const
{
	if( tyinfo ) return tyinfo->hint();
	return hint;
}

llvm::Value* value_t::load( abis abi ) const{
	assert( abi != abi_unknown );

	if( abi != get_abi() ){
		if( is_scalar( get_hint() ) ){
			return load();
		} else if( is_vector( get_hint() ) ){
			Value* org_value = load();
			
			value_t ret_value = cg->null_value( get_hint(), abi );
				
			size_t vec_size = vector_size( get_hint() );
			for( size_t i = 0; i < vec_size; ++i ){
				ret_value = cg->emit_insert_val( ret_value, (int)i, cg->emit_extract_elem(*this, i) );
			}

			return ret_value.load();
		} else if( is_matrix( get_hint() ) ){
			value_t ret_value = cg->null_value( get_hint(), abi );
			size_t vec_count = vector_count( get_hint() );
			for( size_t i = 0; i < vec_count; ++i ){
				value_t org_vec = cg->emit_extract_val( *this, (int)i );
				ret_value = cg->emit_insert_val( ret_value, (int)i, org_vec );
			}
			
			return ret_value.load();
		} else {
			// NOTE: We assume that, if tyinfo is null and hint is none, it is only the entry of vs/ps. Otherwise, tyinfo must be not NULL.
			if( !tyinfo && hint == builtin_types::none ){
				EFLIB_ASSERT_UNIMPLEMENTED();
			} else {
				EFLIB_ASSERT_UNIMPLEMENTED();
			}
		}
		return NULL;
	} else {
		return load();
	}
}

llvm::Value* value_t::load() const{
	Value* ref_val = NULL;
	assert( kind != kind_unknown && kind != kind_tyinfo_only );

	if( kind == kind_ref || kind == kind_value ){
		ref_val = val;
	} else if( (kind&(~kind_ref)) == kind_swizzle ){
		// Decompose indexes.
		char indexes[4] = {-1, -1, -1, -1};
		mask_to_indexes(indexes, masks);
		vector<int> index_values;
		index_values.reserve(4);
		for( int i = 0; i < 4 && indexes[i] != -1; ++i ){
			index_values.push_back( indexes[i] );
		}
		assert( !index_values.empty() );

		// Swizzle
		Value* agg_v = parent->load();

		if( index_values.size() == 1 ){
			// Only one member we could extract reference.
			ref_val = cg->emit_extract_val( parent->to_rvalue(), index_values[0] ).load();
		} else {
			// Multi-members must be swizzle/writemask.
			assert( (kind & kind_ref) == 0 );
			if( abi == abi_c ){
				EFLIB_ASSERT_UNIMPLEMENTED();
				return NULL;
			} else {
				Value* mask = cg->vector_( &indexes[0], index_values.size() );
				return cg->builder().CreateShuffleVector( agg_v, UndefValue::get( agg_v->getType() ), mask );
			}
		}
	} else {
		assert(false);
	}

	if( kind & kind_ref ){
		return cg->builder().CreateLoad( ref_val );
	} else {
		return ref_val;
	}
}

value_t::kinds value_t::get_kind() const{
	return kind;
}

bool value_t::storable() const{
	switch( kind ){
	case kind_ref:
		return true;
	case kind_value:
	case kind_unknown:
		return false;
	default:
		EFLIB_ASSERT_UNIMPLEMENTED();
		return false;
	}
}

bool value_t::load_only() const
{
	switch( kind ){
	case kind_ref:
	case kind_unknown:
		return false;
	case kind_value:
		return true;
	default:
		EFLIB_ASSERT_UNIMPLEMENTED();
		return false;
	}
}

void value_t::emplace( Value* v, kinds k, abis abi ){
	val = v;
	kind = k;
	this->abi = abi;
}

void value_t::emplace( value_t const& v )
{
	emplace( v.raw(), v.get_kind(), v.get_abi() );
}

llvm::Value* value_t::load_ref() const
{
	if( kind == kind_ref ){
		return val;
	} else if( kind == (kind_swizzle|kind_ref) ){
		value_t non_ref( *this );
		non_ref.kind = kind_swizzle;
		return non_ref.load();
	} if( kind == kind_swizzle ){
		assert( masks );
		return cg->emit_extract_elem_mask( *parent, masks ).load_ref();
	}
	return NULL;
}

value_tyinfo* value_t::get_tyinfo() const{
	return tyinfo;
}

value_t& value_t::operator=( value_t const& rhs )
{
	kind = rhs.kind;
	val = rhs.val;
	tyinfo = rhs.tyinfo;
	hint = rhs.hint;
	abi = rhs.abi;
	cg = rhs.cg;
	masks = rhs.masks;

	set_parent(rhs.parent.get());

	return *this;
}

void value_t::set_parent( value_t const& v )
{
	parent.reset( new value_t(v) );
}

void value_t::set_parent( value_t const* v )
{
	if(v){
		set_parent(*v);
	}
}

value_t value_t::slice( value_t const& vec, uint32_t masks )
{
	builtin_types hint = vec.get_hint();
	assert( is_vector(hint) );

	value_t ret( scalar_of(hint), NULL, kind_swizzle, vec.abi, vec.cg );
	ret.masks = masks;
	ret.set_parent(vec);

	return ret;
}

value_t value_t::as_ref() const
{
	value_t ret(*this);
	
	switch( ret.kind ){
	case kind_value:
		ret.kind = kind_ref;
		break;
	case kind_swizzle:
		ret.kind = (kinds)( kind_swizzle | kind_ref );
		break;
	}

	return ret;
}

void value_t::store( value_t const& v ) const
{
	Value* src = v.load( abi );
	Value* address = NULL;

	if( kind == kind_ref ){	
		address = val;
	} else if ( kind == kind_swizzle ){
		if( is_vector(parent->get_hint()) ){
			assert( parent->storable() );
			EFLIB_ASSERT_UNIMPLEMENTED();
		} else {
			address = load_ref();
		}
	}

	StoreInst* inst = cg->builder().CreateStore( src, address );
	inst->setAlignment(4);
}

void value_t::set_index( size_t index )
{
	char indexes[4] = { (char)index, -1, -1, -1 };
	masks = indexes_to_mask( indexes );
}

void value_t::set_hint( builtin_types bt )
{
	hint = bt;
}

void value_t::set_abi( abis abi )
{
	this->abi = abi;
}

/// @}

void cgs_sisd::store( value_t& lhs, value_t const& rhs ){
	// TODO: assert( *lhs.get_tyinfo() == *rhs.get_tyinfo() );
	assert( lhs.storable() );
	value_t rv = rhs.to_rvalue();
	StoreInst* inst = NULL;
	switch( lhs.get_kind() ){
	case value_t::kind_ref:
		inst = builder().CreateStore( rv.load( lhs.get_abi() ), lhs.raw() );
		inst->setAlignment(4);
		break;
	case value_t::kind_unknown:
		// Copy directly.
		lhs.emplace( rhs );
		lhs.tyinfo = rhs.tyinfo;
		break;
	default:
		EFLIB_ASSERT_UNIMPLEMENTED();
	}
}

value_t cgs_sisd::cast_ints( value_t const& v, value_tyinfo* dest_tyi )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::cast_i2f( value_t const& v, value_tyinfo* dest_tyi )
{
	builtin_types hint_i = v.get_hint();
	builtin_types hint_f = dest_tyi->hint();

	Value* val = NULL;
	if( is_signed(hint_i) ){
		val = builder().CreateSIToFP( v.load(), dest_tyi->llvm_ty(abi_llvm) );
	} else {
		val = builder().CreateUIToFP( v.load(), dest_tyi->llvm_ty(abi_llvm) );
	}

	return create_value( dest_tyi, builtin_types::none, val, value_t::kind_value, abi_llvm );
}

value_t cgs_sisd::cast_f2i( value_t const& v, value_tyinfo* dest_tyi )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::cast_f2f( value_t const& v, value_tyinfo* dest_tyi )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::null_value( value_tyinfo* tyinfo, abis abi )
{
	assert( tyinfo && abi != abi_unknown );
	Type* value_type = tyinfo->llvm_ty(abi);
	assert( value_type );
	return value_t( tyinfo, Constant::getNullValue(value_type), value_t::kind_value, abi, this );
}

value_t cgs_sisd::null_value( builtin_types bt, abis abi )
{
	assert( bt != builtin_types::none );
	Type* valty = get_llvm_type( context(), bt, abi == abi_c );
	value_t val = value_t( bt, Constant::getNullValue( valty ), value_t::kind_value, abi, this );
	return val;
}

value_t cgs_sisd::create_vector( std::vector<value_t> const& scalars, abis abi ){
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

void cgs_sisd::emit_return(){
	builder().CreateRetVoid();
}

void cgs_sisd::emit_return( value_t const& ret_v, abis abi ){
	if( abi == abi_unknown ){ abi = fn().abi(); }

	if( fn().first_arg_is_return_address() ){
		builder().CreateStore( ret_v.load(abi), fn().return_address() );
		builder().CreateRetVoid();
	} else {
		builder().CreateRet( ret_v.load(abi) );
	}
}

function_t& cgs_sisd::fn(){
	return fn_ctxts.back();
}

void cgs_sisd::push_fn( function_t const& fn ){
	fn_ctxts.push_back(fn);
}

void cgs_sisd::pop_fn(){
	fn_ctxts.pop_back();
}

shared_ptr<value_tyinfo> cgs_sisd::create_tyinfo( shared_ptr<tynode> const& tyn ){
	cgllvm_sctxt* ctxt = node_ctxt(tyn, true);
	if( ctxt->get_tysp() ){
		return ctxt->get_tysp();
	}

	value_tyinfo* ret = new value_tyinfo();
	ret->sty = tyn.get();
	ret->cls = value_tyinfo::unknown_type;

	if( tyn->is_builtin() ){
		ret->llvm_tys[abi_c] = get_llvm_type( context(), tyn->tycode, true );
		ret->llvm_tys[abi_llvm] = get_llvm_type( context(), tyn->tycode, false );
		ret->cls = value_tyinfo::builtin;
	} else {
		ret->cls = value_tyinfo::aggregated;
		
		if( tyn->is_struct() ){
			shared_ptr<struct_type> struct_tyn = tyn->as_handle<struct_type>();

			vector<Type*> c_member_types;
			vector<Type*> llvm_member_types;
			
			BOOST_FOREACH( shared_ptr<declaration> const& decl, struct_tyn->decls){
				
				if( decl->node_class() == node_ids::variable_declaration ){
					shared_ptr<variable_declaration> decl_tyn = decl->as_handle<variable_declaration>();
					shared_ptr<value_tyinfo> decl_tyinfo = create_tyinfo( decl_tyn->type_info->si_ptr<type_info_si>()->type_info() );
					size_t declarator_count = decl_tyn->declarators.size();
					// c_member_types.insert( c_member_types.end(), (Type*)NULL );
					c_member_types.insert( c_member_types.end(), declarator_count, decl_tyinfo->llvm_ty(abi_c) );
					llvm_member_types.insert( llvm_member_types.end(), declarator_count, decl_tyinfo->llvm_ty(abi_llvm) );
				}
			}

			StructType* ty_c = StructType::get( context(), c_member_types, true );
			StructType* ty_llvm = StructType::get( context(), llvm_member_types, false );

			ret->llvm_tys[abi_c] = ty_c;
			ret->llvm_tys[abi_llvm] = ty_llvm;

			if( !ty_c->isLiteral() ){
				ty_c->setName( struct_tyn->name->str + ".abi.c" );
			}
			if( !ty_llvm->isLiteral() ){
				ty_llvm->setName( struct_tyn->name->str + ".abi.llvm" );
			}
		} else {
			EFLIB_ASSERT_UNIMPLEMENTED();
		}
	}

	ctxt->data().tyinfo = shared_ptr<value_tyinfo>(ret);
	return ctxt->data().tyinfo;
}

function_t cgs_sisd::fetch_function( shared_ptr<function_type> const& fn_node ){
	
	cgllvm_sctxt* fn_ctxt = node_ctxt( fn_node, false );
	if( fn_ctxt->data().self_fn ){
		return fn_ctxt->data().self_fn;
	}

	function_t ret;
	ret.fnty = fn_node.get();
	ret.c_compatible = fn_node->si_ptr<storage_si>()->c_compatible();

	abis abi = ret.c_compatible ? abi_c : abi_llvm;

	vector<Type*> par_tys;

	Type* ret_ty = node_ctxt( fn_node->retval_type, false )->get_typtr()->llvm_ty( abi );

	ret.ret_void = true;
	if( abi == abi_c ){
		if( fn_node->retval_type->tycode != builtin_types::_void ){
			// If function need C compatible and return value is not void, The first parameter is set to point to return value, and parameters moves right.
			Type* ret_ptr = PointerType::getUnqual( ret_ty );
			par_tys.push_back( ret_ptr );
			ret.ret_void = false;
		}

		ret_ty = Type::getVoidTy( context() );
	}

	// Create function type.
	BOOST_FOREACH( shared_ptr<parameter> const& par, fn_node->params )
	{
		cgllvm_sctxt* par_ctxt = node_ctxt( par, false );
		value_tyinfo* par_ty = par_ctxt->get_typtr();
		assert( par_ty );

//		bool is_ref = par->si_ptr<storage_si>()->is_reference();

		Type* par_llty = par_ty->llvm_ty( abi ); 
		if( ret.c_compatible && !is_scalar(par_ty->hint()) ){
			par_tys.push_back( PointerType::getUnqual( par_llty ) );
		} else {
			par_tys.push_back( par_llty );
		}
	}

	
	FunctionType* fty = FunctionType::get( ret_ty, par_tys, false );

	// Create function
	ret.fn = Function::Create( fty, Function::ExternalLinkage, fn_node->symbol()->mangled_name(), module() );
	
	ret.cg = this;
	return ret;
}

bool cgs_sisd::in_function() const{
	return !fn_ctxts.empty();
}

void cgs_sisd::clean_empty_blocks()
{
	assert( in_function() );

	typedef Function::BasicBlockListType::iterator block_iterator_t;
	block_iterator_t beg = fn().fn->getBasicBlockList().begin();
	block_iterator_t end = fn().fn->getBasicBlockList().end();

	dbg_print_blocks( fn().fn );

	// Remove useless blocks
	vector<BasicBlock*> useless_blocks;

	for( block_iterator_t it = beg; it != end; ++it )
	{
		// If block has terminator, that's a well-formed block.
		if( it->getTerminator() ) continue;
		
		// Add no-pred & empty block to remove list.
		if( llvm::pred_begin(it) == llvm::pred_end(it) && it->empty() ){
			useless_blocks.push_back( it );
			continue;
		}
	}

	BOOST_FOREACH( BasicBlock* bb, useless_blocks ){
		bb->removeFromParent();
	}

	// Relink unlinked blocks
	beg = fn().fn->getBasicBlockList().begin();
	end = fn().fn->getBasicBlockList().end();
	for( block_iterator_t it = beg; it != end; ++it ){
		if( it->getTerminator() ) continue;

		// Link block to next.
		block_iterator_t next_it = it;
		++next_it;
		builder().SetInsertPoint( it );
		if( next_it != fn().fn->getBasicBlockList().end() ){
			builder().CreateBr( next_it );
		} else {
			if( !fn().fn->getReturnType()->isVoidTy() ){
				builder().CreateRet( Constant::getNullValue( fn().fn->getReturnType() ) );
			} else {
				emit_return();
			}
		}
	}
}

value_t cgs_sisd::create_scalar( Value* val, value_tyinfo* tyinfo ){
	return value_t( tyinfo, val, value_t::kind_value, abi_llvm, this );
}

insert_point_t cgs_sisd::new_block( std::string const& hint, bool set_as_current )
{
	assert( in_function() );

	insert_point_t ret;
	
	ret.block = BasicBlock::Create( context(), hint, fn().fn );
	dbg_print_blocks( fn().fn );
	
	if( set_as_current ){
		set_insert_point( ret );
	}

	return ret;
}

value_t cgs_sisd::create_value( value_tyinfo* tyinfo, Value* val, value_t::kinds k, abis abi ){
	return value_t( tyinfo, val, k, abi, this );
}

value_t cgs_sisd::create_value( builtin_types hint, Value* val, value_t::kinds k, abis abi )
{
	return value_t( hint, val, k, abi, this );
}

value_t cgs_sisd::create_value( value_tyinfo* tyinfo, builtin_types hint, Value* val, value_t::kinds k, abis abi )
{
	if( tyinfo ){
		return create_value( tyinfo, val, k, abi );
	} else {
		return create_value( hint, val, k ,abi );
	}
}

sasl::code_generator::value_t cgs_sisd::emit_mul( value_t const& lhs, value_t const& rhs )
{
	builtin_types lhint = lhs.get_hint();
	builtin_types rhint = rhs.get_hint();

	if( is_scalar(lhint) ){
		if( is_scalar(rhint) ){
			return emit_mul_ss_vv( lhs, rhs );
		} else if ( is_vector(rhint) ){
			EFLIB_ASSERT_UNIMPLEMENTED();
		} else if ( is_matrix(rhint) ){
			EFLIB_ASSERT_UNIMPLEMENTED();
		}
	} else if ( is_vector(lhint) ){
		if( is_scalar(rhint) ){
			EFLIB_ASSERT_UNIMPLEMENTED();
		} else if ( is_vector(rhint) ){
			emit_mul_ss_vv( lhs, rhs );
		} else if ( is_matrix(rhint) ){
			return emit_mul_vm( lhs, rhs );
		}
	} else if ( is_matrix(lhint) ){
		if( is_scalar(rhint) ){
			EFLIB_ASSERT_UNIMPLEMENTED();
		} else if( is_vector(rhint) ){
			return emit_mul_mv( lhs, rhs );
		} else if( is_matrix(rhint) ){
			return emit_mul_mm( lhs, rhs );
		}
	}

	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

sasl::code_generator::value_t cgs_sisd::emit_add( value_t const& lhs, value_t const& rhs )
{
	builtin_types hint = lhs.get_hint();

	assert( hint != builtin_types::none );
	assert( is_scalar( scalar_of( hint ) ) );
	assert( hint == rhs.get_hint() );

	if( is_scalar(hint) || is_vector(hint) ){
		return emit_add_ss_vv(lhs, rhs);
	}

	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::emit_add_ss_vv( value_t const& lhs, value_t const& rhs )
{
	EMIT_OP_SS_VV_BODY(Add);
}

void cgs_sisd::set_insert_point( insert_point_t const& ip ){
	builder().SetInsertPoint(ip.block);
}

value_t cgs_sisd::emit_dot( value_t const& lhs, value_t const& rhs )
{
	return emit_dot_vv(lhs, rhs);
}

value_t cgs_sisd::emit_dot_vv( value_t const& lhs, value_t const& rhs )
{
	size_t vec_size = vector_size( lhs.get_hint() );

	value_t total = null_value( scalar_of( lhs.get_hint() ), abi_llvm );

	for( size_t i = 0; i < vec_size; ++i ){
		value_t lhs_elem = emit_extract_elem( lhs, i );
		value_t rhs_elem = emit_extract_elem( rhs, i );

		value_t elem_mul = emit_mul_ss_vv( lhs_elem, rhs_elem );
		total.emplace( emit_add_ss_vv( total, elem_mul ).to_rvalue() );
	}

	return total;
}

value_t cgs_sisd::emit_extract_ref( value_t const& lhs, int idx )
{
	assert( lhs.storable() );

	builtin_types agg_hint = lhs.get_hint();

	if( is_vector(agg_hint) ){
		char indexes[4] = { (char)idx, -1, -1, -1 };
		uint32_t mask = indexes_to_mask( indexes );
		return value_t::slice( lhs, mask );
	} else if( is_matrix(agg_hint) ){
		EFLIB_ASSERT_UNIMPLEMENTED();
		return value_t();
	} else if ( agg_hint == builtin_types::none ){
		Value* agg_address = lhs.load_ref();
		Value* elem_address = builder().CreateStructGEP( agg_address, (unsigned)idx );
		value_tyinfo* tyinfo = NULL;
		if( lhs.get_tyinfo() ){
			tyinfo = member_tyinfo( lhs.get_tyinfo(), (size_t)idx );
		}
		return value_t( tyinfo, elem_address, value_t::kind_ref, lhs.get_abi(), this );
	}
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::emit_extract_ref( value_t const& lhs, value_t const& idx )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::emit_extract_val( value_t const& lhs, int idx )
{
	builtin_types agg_hint = lhs.get_hint();

	Value* val = lhs.load();
	Value* elem_val = NULL;
	abis abi = abi_unknown;

	builtin_types elem_hint = builtin_types::none;
	value_tyinfo* elem_tyi = NULL;

	if( agg_hint == builtin_types::none ){
		elem_val = builder().CreateExtractValue(val, static_cast<unsigned>(idx));
		abi = lhs.get_abi();
		elem_tyi = member_tyinfo( lhs.get_tyinfo(), (size_t)idx );
	} else if( is_scalar(agg_hint) ){
		assert( idx == 0 );
		elem_val = val;
		elem_hint = agg_hint;
	} else if( is_vector(agg_hint) ){
		switch( lhs.get_abi() ){
		case abi_c:
			elem_val = builder().CreateExtractValue(val, static_cast<unsigned>(idx));
			break;
		case abi_llvm:
			elem_val = builder().CreateExtractElement(val, int_(idx) );
			break;
		default:
			assert(!"Unknown ABI");
			break;
		}
		abi = abi_llvm;
		elem_hint = scalar_of(agg_hint);
	} else if( is_matrix(agg_hint) ){
		elem_val = builder().CreateExtractValue(val, static_cast<unsigned>(idx));
		abi = lhs.get_abi();
		elem_hint = vector_of( scalar_of(agg_hint), vector_size(agg_hint) );
	}

	// assert( elem_tyi || elem_hint != builtin_types::none );

	if( elem_tyi ){
		return value_t(elem_tyi, elem_val, value_t::kind_value, abi, this );
	} else {
		return value_t(elem_hint, elem_val, value_t::kind_value, abi, this );
	}
	
}

value_t cgs_sisd::emit_extract_val( value_t const& lhs, value_t const& idx )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::emit_mul_ss_vv( value_t const& lhs, value_t const& rhs )
{
	EMIT_OP_SS_VV_BODY(Mul);
}

value_t cgs_sisd::emit_call( function_t const& fn, vector<value_t> const& args )
{
	vector<Value*> arg_values;
	value_t var;
	if( fn.c_compatible ){
		// 
		if ( fn.first_arg_is_return_address() ){
			var = create_variable( fn.get_return_ty().get(), abi_c, "ret" );
			arg_values.push_back( var.load_ref() );
		}

		BOOST_FOREACH( value_t const& arg, args ){
			builtin_types hint = arg.get_hint();
			if( is_scalar(hint) ){
				arg_values.push_back( arg.load(abi_llvm) );
			} else {
				EFLIB_ASSERT_UNIMPLEMENTED();
			}
			// arg_values.push_back( arg.load( abi_llvm ) );
		}
	} else {
		BOOST_FOREACH( value_t const& arg, args ){
			arg_values.push_back( arg.load( abi_llvm ) );
		}
	}

	Value* ret_val = builder().CreateCall( fn.fn, arg_values );

	if( fn.first_arg_is_return_address() ){
		return var;
	}

	abis ret_abi = fn.c_compatible ? abi_c : abi_llvm;
	return value_t( fn.get_return_ty().get(), ret_val, value_t::kind_value, ret_abi, this );
}

value_t cgs_sisd::emit_insert_val( value_t const& lhs, value_t const& idx, value_t const& elem_value )
{
	Value* indexes[1] = { idx.load() };
	Value* agg = lhs.load();
	Value* new_value = NULL;
	if( agg->getType()->isStructTy() ){
		assert(false);
	} else if ( agg->getType()->isVectorTy() ){
		new_value = builder().CreateInsertElement( agg, elem_value.load(), indexes[0] );
	}
	assert(new_value);

	if( lhs.get_tyinfo() ){
		return value_t( lhs.get_tyinfo(), new_value, value_t::kind_value, lhs.get_abi(), this );
	} else {
		return value_t( lhs.get_hint(), new_value, value_t::kind_value, lhs.get_abi(), this );
	}
}

value_t cgs_sisd::emit_insert_val( value_t const& lhs, int index, value_t const& elem_value )
{
	Value* agg = lhs.load();
	Value* new_value = NULL;
	if( agg->getType()->isStructTy() ){
		new_value = builder().CreateInsertValue( agg, elem_value.load(lhs.get_abi()), (unsigned)index );
	} else if ( agg->getType()->isVectorTy() ){
		value_t index_value( builtin_types::_sint32, int_(index), value_t::kind_value, abi_llvm, this );
		return emit_insert_val( lhs, index_value, elem_value );
	}
	assert(new_value);

	if( lhs.get_tyinfo() ){
		return value_t( lhs.get_tyinfo(), new_value, value_t::kind_value, lhs.get_abi(), this );
	} else {
		return value_t( lhs.get_hint(), new_value, value_t::kind_value, lhs.get_abi(), this );
	}

	
}

value_t cgs_sisd::emit_mul_mv( value_t const& lhs, value_t const& rhs )
{
	builtin_types mhint = lhs.get_hint();
	builtin_types vhint = rhs.get_hint();

	size_t row_count = vector_count(mhint);
	size_t vec_size = vector_size(mhint);

	builtin_types ret_hint = vector_of( scalar_of(vhint), row_count );

	value_t ret_v = null_value( ret_hint, lhs.abi );
	for( size_t irow = 0; irow < row_count; ++irow ){
		value_t row_vec = emit_extract_val( lhs, irow );
		ret_v = emit_insert_val( ret_v, irow, emit_dot_vv(row_vec, rhs) );
	}

	return ret_v;
}

value_t cgs_sisd::emit_mul_mm( value_t const& lhs, value_t const& rhs )
{
	builtin_types lhint = lhs.get_hint();
	builtin_types rhint = rhs.get_hint();

	size_t out_v = vector_size( lhint );
	size_t out_r = vector_count( rhint );
	size_t inner_size = vector_size(rhint);

	builtin_types out_row_hint = vector_of( scalar_of(lhint), out_v );
	builtin_types out_hint = matrix_of( scalar_of(lhint), out_v, out_r );
	abis out_abi = lhs.abi;

	vector<value_t> out_cells(out_v*out_r);
	out_cells.resize( out_v*out_r );

	// Caluclate matrix cells.
	for( size_t icol = 0; icol < out_v; ++icol){
		value_t col = emit_extract_col( rhs, icol );
		for( size_t irow = 0; irow < out_r; ++irow )
		{
			value_t row = emit_extract_col( rhs, icol );
			out_cells[irow*out_v+icol] = emit_dot_vv( col, row );
		}
	}

	// Compose cells to matrix
	value_t ret_value = null_value( out_hint, out_abi );
	for( size_t irow = 0; irow < out_r; ++irow ){
		value_t row_vec = null_value( out_row_hint, out_abi );
		for( size_t icol = 0; icol < out_v; ++icol ){
			row_vec = emit_insert_val( row_vec, (int)icol, out_cells[irow*out_v+icol] );
		}
		ret_value = emit_insert_val( ret_value, (int)irow, row_vec );
	}

	return ret_value;
}

value_t cgs_sisd::emit_extract_col( value_t const& lhs, size_t index )
{
	value_t val = lhs.to_rvalue();
	builtin_types mat_hint( lhs.get_hint() );
	assert( is_matrix(mat_hint) );

	size_t row_count = vector_count( mat_hint );
	
	builtin_types out_hint = vector_of( scalar_of(mat_hint), row_count );

	value_t out_value = null_value( out_hint, lhs.abi );
	for( size_t irow = 0; irow < row_count; ++irow ){
		value_t row = emit_extract_val( val, (int)irow );
		value_t cell = emit_extract_val( row, (int)index );
		out_value = emit_insert_val( out_value, (int)irow, cell );
	}

	return out_value;
}

value_t cgs_sisd::emit_mul_vm( value_t const& lhs, value_t const& rhs )
{
	size_t out_v = vector_size( rhs.get_hint() );

	value_t lrv = lhs.to_rvalue();
	value_t rrv = rhs.to_rvalue();

	value_t ret = null_value( vector_of( scalar_of(lhs.get_hint()), out_v ), lhs.get_abi() );
	for( size_t idx = 0; idx < out_v; ++idx ){
		ret = emit_insert_val( ret, (int)idx, emit_dot_vv( lrv, emit_extract_col(rrv, idx) ) );
	}

	return ret;
}

llvm::Type* cgs_sisd::type_( builtin_types bt, abis abi )
{
	assert( abi != abi_unknown );
	return get_llvm_type( context(), bt, abi == abi_c );
}

Type* cgs_sisd::type_( value_tyinfo const* ty, abis abi )
{
	assert( ty->llvm_ty(abi) );
	return ty->llvm_ty(abi);
}

value_t cgs_sisd::create_variable( builtin_types bt, abis abi, std::string const& name )
{
	Type* var_ty = type_( bt, abi );
	Value* var_val = builder().CreateAlloca( var_ty );
	return value_t( bt, var_val, value_t::kind_ref, abi, this );
}

value_t cgs_sisd::create_variable( value_tyinfo const* ty, abis abi, std::string const& name )
{
	Type* var_ty = type_(ty, abi);
	Value* var_val = builder().CreateAlloca( var_ty );
	return value_t( const_cast<value_tyinfo*>(ty), var_val, value_t::kind_ref, abi, this );
}

value_tyinfo* cgs_sisd::member_tyinfo( value_tyinfo const* agg, size_t index ) const
{
	if( !agg ){
		return NULL;
	} else if ( agg->typtr()->is_struct() ){
		shared_ptr<struct_type> struct_sty = agg->typtr()->as_handle<struct_type>();
		
		size_t var_index = 0;
		BOOST_FOREACH( shared_ptr<declaration> const& child, struct_sty->decls ){
			if( child->node_class() == node_ids::variable_declaration ){
				shared_ptr<variable_declaration> vardecl = child->as_handle<variable_declaration>();
				var_index += vardecl->declarators.size();
				if( index < var_index ){
					return const_cast<cgs_sisd*>(this)->node_ctxt( vardecl, false )->get_typtr();
				}
			}
		}

		assert(!"Out of struct bound.");
	} else {
		EFLIB_ASSERT_UNIMPLEMENTED();
	}

	return NULL;
}

value_t cgs_sisd::emit_extract_elem_mask( value_t const& vec, uint32_t mask )
{
	char indexes[4] = {-1, -1, -1, -1};
	mask_to_indexes( indexes, mask );
	assert( indexes[0] != -1 );
	if( indexes[1] == -1 ){
		return emit_extract_elem( vec, indexes[0] );
	} else {
		// Caculate out size
		/*int out_size = 0;
		for( int i = 0; i < 4; ++i ){
			if( indexes[i] == -1 ){ break; }
			++out_size;
		}*/

		EFLIB_ASSERT_UNIMPLEMENTED();

		return value_t();
	}
}

value_t cgs_sisd::emit_sub_ss_vv( value_t const& lhs, value_t const& rhs )
{
	EMIT_OP_SS_VV_BODY( Sub );
}

value_t cgs_sisd::emit_sub( value_t const& lhs, value_t const& rhs )
{
	builtin_types hint = lhs.get_hint();

	assert( hint != builtin_types::none );
	assert( is_scalar( scalar_of( hint ) ) );
	assert( hint == rhs.get_hint() );

	if( is_scalar(hint) || is_vector(hint) ){
		return emit_sub_ss_vv(lhs, rhs);
	}

	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

insert_point_t cgs_sisd::insert_point() const
{
	insert_point_t ret;
	ret.block = builder().GetInsertBlock();
	return ret;
}

void cgs_sisd::jump_to( insert_point_t const& ip )
{
	assert( ip );
	if( !insert_point().block->getTerminator() ){
		builder().CreateBr( ip.block );
	}
}

void cgs_sisd::jump_cond( value_t const& cond_v, insert_point_t const const & true_ip, insert_point_t const& false_ip )
{
	Value* cond = cond_v.load();
	builder().CreateCondBr( cond, true_ip.block, false_ip.block );
}


Value* cgs_sisd::select_( Value* cond, Value* yes, Value* no )
{
	return builder().CreateSelect( cond, yes, no );
}

value_t cgs_sisd::emit_cmp_eq( value_t const& lhs, value_t const& rhs )
{
	EMIT_CMP_EQ_NE_BODY( EQ );
}

value_t cgs_sisd::emit_cmp_lt( value_t const& lhs, value_t const& rhs )
{
	EMIT_CMP_BODY( LT );
}

value_t cgs_sisd::emit_cmp_le( value_t const& lhs, value_t const& rhs )
{
	EMIT_CMP_BODY( LE );
}

value_t cgs_sisd::emit_cmp_ne( value_t const& lhs, value_t const& rhs )
{
	EMIT_CMP_EQ_NE_BODY( NE );
}

value_t cgs_sisd::emit_cmp_ge( value_t const& lhs, value_t const& rhs )
{
	EMIT_CMP_BODY( GE );
}

value_t cgs_sisd::emit_cmp_gt( value_t const& lhs, value_t const& rhs )
{
	EMIT_CMP_BODY( GT );
}

value_t cgs_sisd::emit_sqrt( value_t const& arg_value )
{
	builtin_types hint = arg_value.get_hint();
	builtin_types scalar_hint = scalar_of( arg_value.get_hint() );
	if( is_scalar(hint) ){
		if( hint == builtin_types::_float ){
			if( prefer_externals() ) {
				EFLIB_ASSERT_UNIMPLEMENTED();
				//	function_t fn = external_proto( &externals::sqrt_f );
				//	vector<value_t> args;
				//	args.push_back(lhs);
				//	return emit_call( fn, args );
			} else if( support_feature( cpu_sse2 ) && !prefer_scalar_code() ){
				// Extension to 4-elements vector.
				value_t v4 = undef_value( vector_of(scalar_hint, 4), abi_llvm );
				v4 = emit_insert_val( v4, 0, arg_value );
				Value* v = builder().CreateCall( intrin_( Intrinsic::x86_sse_sqrt_ss ), v4.load() );
				Value* ret = builder().CreateExtractElement( v, int_(0) );

				return create_value( arg_value.get_tyinfo(), hint, ret, value_t::kind_value, abi_llvm );
			} else {
				// Emit LLVM intrinsics
				Value* v = builder().CreateCall( intrin_<float(float)>(Intrinsic::sqrt), arg_value.load() );
				return create_value( arg_value.get_tyinfo(), arg_value.get_hint(), v, value_t::kind_value, abi_llvm );
			}
		} else if( hint == builtin_types::_double ){
			EFLIB_ASSERT_UNIMPLEMENTED();
		} 
	} else if( is_vector(hint) ) {

		size_t vsize = vector_size(hint);

		if( scalar_hint == builtin_types::_float ){
			if( prefer_externals() ){
				EFLIB_ASSERT_UNIMPLEMENTED();
			} else if( support_feature(cpu_sse2) && !prefer_scalar_code() ){
				// TODO emit SSE2 instrinsic directly.

				// expanded to vector 4
				value_t v4;
				if( vsize == 4 ){	
					v4 = create_value( arg_value.get_tyinfo(), hint, arg_value.load(abi_llvm), value_t::kind_value, abi_llvm );
				} else {
					v4 = null_value( vector_of( scalar_hint, 4 ), abi_llvm );
					for ( size_t i = 0; i < vsize; ++i ){
						v4 = emit_insert_val( v4, i, emit_extract_elem(arg_value, i) );
					}
				}
				
				// calculate
				Value* v = builder().CreateCall( intrin_( Intrinsic::x86_sse_sqrt_ps ), v4.load() );

				if( vsize < 4 ){
					// Shrink
					static int indexes[4] = {0, 1, 2, 3};
					Value* mask = vector_( &indexes[0], vsize );
					v = builder().CreateShuffleVector( v, UndefValue::get( v->getType() ), mask );
				}

				return create_value( NULL, hint, v, value_t::kind_value, abi_llvm );
			} else {
				value_t ret = null_value( hint, arg_value.get_abi() );
				for( size_t i = 0; i < vsize; ++i ){
					value_t elem = emit_extract_elem( arg_value, i );
					ret = emit_insert_val( ret, i, emit_sqrt( arg_value ) );
				}
				return ret;
			}
		} else {
			EFLIB_ASSERT_UNIMPLEMENTED();
		}
	} else {
		EFLIB_ASSERT_UNIMPLEMENTED();
	}

	return value_t();
}

bool cgs_sisd::prefer_externals() const
{
	return false;
}

bool cgs_sisd::prefer_scalar_code() const
{
	return false;
}

Function* cgs_sisd::intrin_( int v )
{
	return intrins.get( llvm::Intrinsic::ID(v), module() );
}

value_t cgs_sisd::undef_value( builtin_types bt, abis abi )
{
	assert( bt != builtin_types::none );
	Type* valty = get_llvm_type( context(), bt, abi == abi_c );
	value_t val = value_t( bt, UndefValue::get(valty), value_t::kind_value, abi, this );
	return val;
}

value_t cgs_sisd::emit_cross( value_t const& lhs, value_t const& rhs )
{
	assert( lhs.get_hint() == vector_of( builtin_types::_float, 3 ) );
	assert( rhs.get_hint() == lhs.get_hint() );

	int swz_a[] = {1, 2, 0};
	int swz_b[] = {2, 0, 1};

	ConstantVector* swz_va = vector_( swz_a, 3 );
	ConstantVector* swz_vb = vector_( swz_b, 3 );

	Value* lvec_value = lhs.load(abi_llvm);
	Value* rvec_value = rhs.load(abi_llvm);

	Value* lvec_a = builder().CreateShuffleVector( lvec_value, UndefValue::get( lvec_value->getType() ), swz_va );
	Value* lvec_b = builder().CreateShuffleVector( lvec_value, UndefValue::get( lvec_value->getType() ), swz_vb );
	Value* rvec_a = builder().CreateShuffleVector( rvec_value, UndefValue::get( rvec_value->getType() ), swz_va );
	Value* rvec_b = builder().CreateShuffleVector( rvec_value, UndefValue::get( rvec_value->getType() ), swz_vb );

	Value* mul_first = builder().CreateFMul( lvec_a, rvec_b );
	Value* mul_second = builder().CreateFMul( lvec_b, rvec_a );

	Value* ret = builder().CreateFSub( mul_first, mul_second );

	return create_value( lhs.get_tyinfo(), lhs.get_hint(), ret, value_t::kind_value, abi_llvm );
}

value_t cgs_sisd::emit_swizzle( value_t const& lhs, uint32_t mask )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

value_t cgs_sisd::emit_write_mask( value_t const& vec, uint32_t mask )
{
	EFLIB_ASSERT_UNIMPLEMENTED();
	return value_t();
}

void cgs_sisd::switch_to( value_t const& cond, std::vector< std::pair<value_t, insert_point_t> > const& cases, insert_point_t const& default_branch )
{
	Value* v = cond.load();
	SwitchInst* inst = builder().CreateSwitch( v, default_branch.block, static_cast<unsigned>(cases.size()) );
	for( size_t i_case = 0; i_case < cases.size(); ++i_case ){
		inst->addCase( llvm::cast<ConstantInt>( cases[i_case].first.load() ), cases[i_case].second.block );
	}
}

value_t cgs_sisd::cast_i2b( value_t const& v )
{
	assert( is_integer(v.get_hint()) );
	return emit_cmp_ne( v, null_value( v.get_hint(), v.get_abi() ) );
}

value_t cgs_sisd::cast_f2b( value_t const& v )
{
	assert( is_real(v.get_hint()) );
	return emit_cmp_ne( v, null_value( v.get_hint(), v.get_abi() ) );
}

cgllvm_sctxt* cgs_sisd::node_ctxt( shared_ptr<node> const& node, bool create_if_need )
{
	return cg_service::node_ctxt( node.get(), create_if_need );
}

void function_t::arg_name( size_t index, std::string const& name ){
	size_t param_size = fn->arg_size();
	if( first_arg_is_return_address() ){
		--param_size;
	}
	assert( index < param_size );

	Function::arg_iterator arg_it = fn->arg_begin();
	if( first_arg_is_return_address() ){
		++arg_it;
	}

	for( size_t i = 0; i < index; ++i ){ ++arg_it; }
	arg_it->setName( name );
}

void function_t::args_name( vector<string> const& names )
{
	Function::arg_iterator arg_it = fn->arg_begin();
	vector<string>::const_iterator name_it = names.begin();

	if( first_arg_is_return_address() ){
		arg_it->setName(".ret");
		++arg_it;
	}

	for( size_t i = 0; i < names.size(); ++i ){
		arg_it->setName( *name_it );
		++arg_it;
		++name_it;
	}
}

shared_ptr<value_tyinfo> function_t::get_return_ty() const{
	assert( fnty->is_function() );
	return shared_ptr<value_tyinfo>( cg->node_ctxt( fnty->retval_type, false )->get_tysp() );
}

size_t function_t::arg_size() const{
	assert( fn );
	if( fn ){
		if( first_arg_is_return_address() ){ return fn->arg_size() - 1; }
		return fn->arg_size() - 1;
	}
	return 0;
}

value_t function_t::arg( size_t index ) const
{
	// If c_compatible and not void return, the first argument is address of return value.
	size_t arg_index = index;
	if( first_arg_is_return_address() ){ ++arg_index; }

	shared_ptr<parameter> par = fnty->params[index];
	value_tyinfo* par_typtr = cg->node_ctxt( par, false )->get_typtr();

	Function::ArgumentListType::iterator it = fn->arg_begin();
	for( size_t idx_counter = 0; idx_counter < arg_index; ++idx_counter ){
		++it;
	}

	abis arg_abi = c_compatible ? abi_c: abi_llvm;
	return cg->create_value( par_typtr, &(*it), arg_is_ref(index) ? value_t::kind_ref : value_t::kind_value, arg_abi );
}

function_t::function_t(): fn(NULL), fnty(NULL), ret_void(true), c_compatible(false), cg(NULL)
{
}

bool function_t::arg_is_ref( size_t index ) const{
	assert( index < fnty->params.size() );

	builtin_types hint = fnty->params[index]->si_ptr<storage_si>()->type_info()->tycode;
	return c_compatible && !is_scalar(hint);
}

bool function_t::first_arg_is_return_address() const
{
	return c_compatible && !ret_void;
}

abis function_t::abi() const
{
	return c_compatible ? abi_c : abi_llvm;
}

llvm::Value* function_t::return_address() const
{
	if( first_arg_is_return_address() ){
		return &(*fn->arg_begin());
	}
	return NULL;
}

void function_t::return_name( std::string const& s )
{
	if( first_arg_is_return_address() ){
		fn->arg_begin()->setName( s );
	}
}

void function_t::inline_hint()
{
	fn->addAttribute( 0, llvm::Attribute::InlineHint );
}

insert_point_t::insert_point_t(): block(NULL)
{
}

END_NS_SASL_CODE_GENERATOR();