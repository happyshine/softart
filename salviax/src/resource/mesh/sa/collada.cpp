#include <salviax/include/resource/mesh/sa/collada.h>

#include <eflib/include/platform/boost_begin.h>
#include <boost/foreach.hpp>
#include <eflib/include/platform/boost_end.h>

#include <sstream>

using boost::property_tree::ptree;
using boost::make_shared;
using boost::unordered_map;
using boost::optional;

using std::string;
using std::stringstream;

BEGIN_NS_SALVIAX_RESOURCE();

void dae_node::parse_attribute( ptree& xml_node, dae_node_ptr node, dae_dom_ptr file )
{
	node->id		= xml_node.get_optional<string>("<xmlattr>.id");
	node->sid		= xml_node.get_optional<string>("<xmlattr>.sid");
	node->name		= xml_node.get_optional<string>("<xmlattr>.name");
	node->source	= xml_node.get_optional<string>("<xmlattr>.source");
	node->root		= file.get();

	if( node->id )
	{
		file->id_nodes.insert( make_pair(*node->id, node) );
	}
}

dae_tech_ptr dae_tech::parse( ptree& root, dae_dom_ptr file )
{
	dae_tech_ptr ret = make_shared<dae_tech>();
	parse_attribute(root, ret, file);

	optional<ptree&> accessor_xml_node = root.get_child_optional("accessor");

	if( !accessor_xml_node ) {
		ret.reset(); 
	} else {
		ret->accessor = dae_accessor::parse( *accessor_xml_node, file );
	}

	return ret;
}

dae_accessor_ptr dae_accessor::parse( ptree& root, dae_dom_ptr file )
{
	dae_accessor_ptr ret = make_shared<dae_accessor>();
	parse_attribute(root, ret, file);

	ret->source_array = file->get_node<dae_array>(*ret->source);

	if( ret->source_array )
	{
		ret->offset = root.get( "<xmlattr>.offset", size_t(0) );
		ret->stride = root.get( "<xmlattr>.stride", size_t(0) );
		ret->count  = root.get( "<xmlattr>.count" , size_t(0) );

		BOOST_FOREACH( ptree::value_type& child, root )
		{
			if( child.first == "param" )
			{
				ret->params.push_back( dae_param::parse(child.second, file) );
			}
		}
	}
	else
	{
		ret.reset();
	}

	return ret;
}

dae_mesh_ptr dae_mesh::parse( ptree& root, dae_dom_ptr file )
{
	dae_mesh_ptr ret = make_shared<dae_mesh>();

	parse_attribute(root, ret, file);

	BOOST_FOREACH( ptree::value_type& child, root )
	{
		if( child.first == "<xmlattr>" ) { continue; }

		if( child.first == "source" ){
			ret->sources.push_back( dae_source::parse(child.second, file) );
		} else if( child.first == "vertices" ) {
			ret->verts.push_back( dae_verts::parse(child.second, file) );
		} else if( child.first == "triangles" ) {
			ret->triangle_sets.push_back( dae_triangles::parse(child.second, file) );
		} else {
			assert(false);
		}
	}

	return ret;
}

dae_source_ptr dae_source::parse( ptree& root, dae_dom_ptr file )
{
	dae_source_ptr ret = make_shared<dae_source>();
	parse_attribute(root, ret, file);

	BOOST_FOREACH( ptree::value_type& child, root )
	{
		if( child.first == "<xmlattr>" ) { continue; }

		if(/**/child.first == "float_array"
			|| child.first == "int_array" )
		{
			ret->arr = dae_array::parse(child.first, child.second, file);
		}
		else if( child.first == "technique_common" )
		{
			ret->tech = dae_tech::parse(child.second, file);
		}
		else
		{
			EFLIB_ASSERT_UNIMPLEMENTED();
		}
	}

	return ret;
}

dae_verts_ptr dae_verts::parse( ptree& root, dae_dom_ptr file )
{
	dae_verts_ptr ret = make_shared<dae_verts>();
	parse_attribute(root, ret, file);

	BOOST_FOREACH( ptree::value_type& child, root )
	{
		if( child.first == "<xmlattr>" )
		{
			ret->count = child.second.get("count", (size_t)0);
			ret->material_name = child.second.get_optional<string>("material");
		}
		else if( child.first == "input" )
		{
			ret->inputs.push_back( dae_input::parse(child.second, file) );
		}
	}

	return ret;
}

dae_triangles_ptr dae_triangles::parse( ptree& root, dae_dom_ptr file )
{
	dae_triangles_ptr ret = make_shared<dae_triangles>();
	parse_attribute(root, ret, file);

	BOOST_FOREACH( ptree::value_type& child, root )
	{
		if( child.first == "<xmlattr>" )
		{
			ret->count = child.second.get("count", 0);
			ret->material_name = child.second.get_optional<string>("material");
		}
		else if( child.first == "p" )
		{
			stringstream ss( child.second.get_value<string>() );
			int32_t v = 0;
			while( ss >> v ) { ret->indexes.push_back(v); }
		}
		else if( child.first == "input" )
		{
			ret->inputs.push_back( dae_input::parse(child.second, file) );
		}
	}

	return ret;
}

dae_param_ptr dae_param::parse( ptree& root, dae_dom_ptr file )
{
	dae_param_ptr ret = make_shared<dae_param>();
	parse_attribute(root, ret, file);

	string type_str = root.get<string>("<xmlattr>.type");

	/**/ if(type_str == "float")	{ ret->vtype = salviar::lvt_float; }
	else if(type_str == "int")		{ ret->vtype = salviar::lvt_sint32;}
	else { assert(false); }

	return ret;
}

dae_input_ptr dae_input::parse( ptree& root, dae_dom_ptr file )
{
	dae_input_ptr ret = make_shared<dae_input>();
	parse_attribute(root, ret, file);

	ret->semantic	= root.get_optional<string>("<xmlattr>.semantic");
	ret->offset		= root.get("<xmlattr>.offset",	(size_t)0);
	ret->set		= root.get("<xmlattr>.set",		(size_t)0);

	return ret;
}

bool dae_param::index( string const& index_seq, size_t& idx )
{
	if( !name ) return false;
	idx = index_seq.find( (*name)[0] );
	return idx != string::npos;
}

int dae_param::index( string const& index_seq )
{
	size_t ret = 0;
	if( index(index_seq, ret) )
	{
		return static_cast<int>(ret);
	}
	return -1;
}

int dae_param::index_xyzw_stpq()
{
	int ret = index("XYZW");
	if( ret != -1 ) return ret;
	return index("STPQ");
}

dae_array_ptr dae_array::parse( string const& name, ptree& root, dae_dom_ptr file )
{
	dae_array_ptr ret = make_shared<dae_array>();
	parse_attribute(root, ret, file);

	if( name == "float_array" ) ret->array_type = float_array;
	if( name == "int_array" )	ret->array_type = int_array;

	ret->count = root.get( "<xmlattr>.count", 0 );
	ret->content = root.get_value_optional<string>();

	// Parse elements
	if( ret->content )
	{
		switch (ret->array_type)
		{
		case float_array:
			{
				stringstream ss( *ret->content );
				float v = 0.0f;
				while( ss >> v ) { ret->float_arr.push_back(v); }
				if( ret->count == 0 ) { ret->count = ret->float_arr.size(); }
			}
			break;
		case int_array:
			{
				stringstream ss( *ret->content );
				int v = 0;
				while( ss >> v ) { ret->int_arr.push_back(v); }
				if( ret->count == 0 ) { ret->count = ret->int_arr.size(); }
			}
			break;
		}
	}

	return ret;
}

END_NS_SALVIAX_RESOURCE();