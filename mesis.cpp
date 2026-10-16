#include <iostream>
#include <emscripten.h>
#include <emscripten/val.h>
#include <emscripten/bind.h>
#include <emscripten/html5.h>
#include <vector>
#include <map>
#include <deque>
#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <variant>


using std::string;
using emscripten::val;
#define js_null val::undefined
#define self(p) val::module_property(#p)
#define self_(p) val::module_property(p)


val global_v = val::global( "globalThis" );
val g_ = global_v;
val console = g_[ "console" ];
val Object = g_[ "Object" ];
val window = g_[ "window" ];
val doc = g_[ "document" ];
val html = doc[ "documentElement" ];


// controla contexto de possibilidade de execucao de funcoes - algumas funcoes sao
// expostas para o JS mas somente para uso da interface c++ <-> V8, nao devendo
// ser executadas pelo usuario
std::map< string, string > can_run = {};

// Mantem o objeto global q deve ser criado chamado Mesis que contem o
// html codificado como JS
val html_src_v = js_null();


size_t frame_counter = -1;
uint32_t local_time = 0;


string err_dont_call_directly = "this function should not be called directly";


string s( const char* str )
{
    return string( str );
}


string s( val elem_ )
{
    return elem_.as< string >();
}

// map < function_name, < frame_window, frame_pos >> para inserir funcoes no loop do AnimationFrame
//  frame windows diz de quantos em quantos frames deve rodar a funcao, e pos é o frame dentro da
//  janela onde ela roda. Pode-se tb usar frame_windows negativo para contar exatamente n frames
// e rodar a funcao. Isso é util pq o num do frame % tam janela é usado, entao a 1a exec pode cair
// num intervalo menor do q o tamanho da janela
std::map< string, std::pair< int16_t, int16_t > > on_animation_frame;

// a kv store serve para armazenar conteudo entre contextos (como um contexto global encapsulado)
std::map< string, val > kv_store = {};

std::string trim( std::string_view str )
{
    const std::string_view whitespace = " \t\n\r\f\v";
    const auto strBegin = str.find_first_not_of( whitespace );

    if( strBegin == string::npos ) return "";

    const auto strEnd = str.find_last_not_of( whitespace );
    const auto strRange = strEnd - strBegin + 1;

    // A busca foi feita sem cópias. A cópia só ocorre UMA VEZ aqui no return.
    return string( str.substr( strBegin, strRange ));
}


val jo( std::map< string, string > cpp_o )
{
    val o = g_[ "Object" ].new_();

    for( auto& pair_ : cpp_o )
        o.set( pair_.first.c_str(), pair_.second );

    return o;
}


val jmap( std::map< string, string > cpp_o )
{
    val o = g_[ "Map" ].new_();

    for( auto& pair_ : cpp_o )
        o.call< void >( "set", pair_.first, pair_.second );

    return o;
}


std::vector< string > split( const string& s, char delimiter )
{
    std::vector< string > tokens;
    string token;
    std::istringstream tokenStream( s );
    while( std::getline( tokenStream, token, delimiter ))
    {   tokens.push_back( trim( token ));
    }
    return tokens;
}


std::vector< string > split( const string& s, const string& delimiter )
{
    std::vector< string > tokens;
    size_t start = 0;
    size_t end = s.find( delimiter );

    if( delimiter.empty())
    {
        tokens.push_back( trim( s ));
        return tokens;
    }

    while( end != string::npos )
    {
        tokens.push_back( trim( s.substr( start, end - start )));
        start = end + delimiter.length();
        end = s.find( delimiter, start );
    }

    tokens.push_back( trim( s.substr( start )));

    return tokens;
}


// @note inplace replacement
void replaceAll( string& str, const string& from, const string& to )
{
    if( from.empty())
        return;

    size_t start_pos = 0;
    while(( start_pos = str.find( from, start_pos )) != string::npos )
    {
        str.replace( start_pos, from.length(), to );
        // Avança a posição inicial de busca após a string de substituição
        start_pos += to.length();
    }
}


bool stack_trace = true;


// remove from the can_run list 
void can_run_exclude( string src, string allowed )
{
    // TODO: sustituir as linhas q removem autorizacao de execucao para
    // esta forma qdo for possivel execucoes paralelas
    string run_allow = can_run[ src ];
    string allowed_ = allowed;
    if( allowed_[ 0 ] != '|' )
        allowed_ = s( "|" ) + allowed_ + s( "|" );

    string nRun_allow = run_allow.substr( 0, run_allow.find( allowed_ ));
    nRun_allow += run_allow.substr( run_allow.find( allowed_ ) + allowed_.size());
    if( nRun_allow.size() > 0 )
        can_run[ src ] = nRun_allow;
    else
        can_run.erase( src );
}


string s( std::string_view str )
{
    return string( str );
}


void multi_set( val elem_, std::map< string, string > props )
{
    // 【 context helper 】
    for( auto prop : props )
        elem_.set( prop.first.c_str(), prop.second );
}


struct DomInterface
{
    val inst;

    struct PropProxy
    {
        val& inst;
        std::string key;

        void operator=( DomInterface value )
        {
            bool hasProperty = g_[ "Reflect" ].call< val >( "has", inst, key ).as<bool>();

            if( hasProperty )
                inst.set( key, value.inst );
            else
                // nao eh preciso testar o data- pq o setAttribute ja faz o trabalho sujo
                inst.call< void >( "setAttribute", key, value.inst.as< string >());
        }

        void operator=( val value )
        {
            bool hasProperty = g_[ "Reflect" ].call< val >( "has", inst, key ).as<bool>();

            if( hasProperty )
                inst.set( key, value );
            else
                inst.call< void >( "setAttribute", key, value );
        }

        void operator=( string value )
        {
            bool hasProperty = g_[ "Reflect" ].call< val >( "has", inst, key ).as<bool>();

            if( hasProperty )
                inst.set( key, value );
            else
                inst.call< void >( "setAttribute", key, value );
        }

        val resolve() const
        {
            bool hasProperty = g_[ "Reflect" ].call< val >( "has", inst, key ).as<bool>();

            val p = inst[ key ];

            if( hasProperty == false )
            {
                // atributo ou membro do dataset
                if( key.find( "data-" ) != string::npos )
                {
                    string dataset_key = key;
                    replaceAll( dataset_key, "data-", "" );
                    p = inst[ "dataset" ][ dataset_key ];
                }
                else
                    p = inst.call< val >( "getAttribute", key );
            }

            if( p == js_null())
                return js_null();
            return p;
        }

        bool operator==( val other ) const
        {
            val res = resolve();
            if( res == js_null())
                return ( other == js_null());

            return res == other;
        }

        bool operator!=( val other ) const
        {
            val res = resolve();

            if( res == js_null())
            {   return !( other == js_null());   }
            else
            {
                string type = Object[ "prototype" ][ "toString" ].call< string >( "call", res );
                string other_t = Object[ "prototype" ][ "toString" ].call< string >( "call", other );

                if( type == "[object String]" && other_t == "[object String]" )
                    return res.as< string >() != other.as< string >();

                if( type != "[object String]" && other_t != "[object String]" )
                    return res != other;

                if( other.isUndefined() || other.isNull())
                    return true;

                return res.as< string >() != other.as< string >();
            }
        }

        operator bool() const
        {   return resolve().as< bool >();   }

        operator string() const
        {
            val r = resolve();
            return ( r == js_null() ? "" : r.as< string >());
        }

        operator val() const
        {
            val res = resolve();
            return res;
        }

        template< typename T >
        T as() const
        {   return resolve().as<T>();   }

        string s()
        {
            return resolve().as< string >();
        }
    };

    PropProxy operator<<( string key )
    {
        inst.set( key, val::undefined());
        return PropProxy{ inst, key };
    }

    void operator=( val new_elem )
    {
        inst = new_elem;
    }

    bool operator==( val other ) const
    {   return ( inst == other );   }

    bool operator==( DomInterface other ) const
    {   return ( inst == other.inst );   }

    bool operator!=( val other ) const
    {   return ( inst != other );   }

    bool operator!=( DomInterface other ) const
    {   return ( inst != other.inst );   }

    void appendChild( val child ) { inst.call< void >( "appendChild", child ); }
    void appendChild( DomInterface child ) { inst.call< void >( "appendChild", child.inst ); }

    DomInterface clone() { DomInterface res = { inst.call< val >( "cloneNode", true )}; return res; }

    string id() { return s( inst[ "id" ]); }

    val el_( string selector ) { return inst.call< val >( "elem", selector ); }

    std::vector< string > keys()
    {
        val target_keys = Object.call< val >( "keys", inst );
        return emscripten::vecFromJSArray< string >( target_keys );
    }

    void replaceChildren( val repl )
    {   string is_coll = Object[ "prototype" ][ "toString" ].call< string >( "call", repl );
        if( is_coll.find( "ollection]" ) != string::npos || is_coll.find( "rray]" ) != string::npos )
        {   int len = repl[ "length" ].as< int >();
            clear();
            for( int c = 0; c < len; c++ )
                inst.call< void >( "append", repl[ c ]);
        }
        else
            inst.call< val >( "replaceChildren", repl ); 
    }

    void clear() { inst.call< val >( "replaceChildren" ); }

    void clear( val to_erase ) 
    {   string is_array = Object[ "prototype" ][ "toString" ].call< string >( "call", to_erase );
        if( is_array.find( "rray]" ) == string::npos )
        {   val wrap = g_[ "Array" ].new_();
            wrap.call< void >( "push", to_erase );
            to_erase = wrap;
        }
        int len = to_erase[ "length" ].as< int >();

        for( int i = len - 1; i >= 0; i-- )
            to_erase[ i ].call< void >( "remove" ); 
    }

    void replaceWith( val rpmt )
    {
        if( rpmt[ "fill" ] != js_null() && rpmt[ "every" ] != js_null())
            inst[ "replaceWith" ][ "apply" ]( rpmt );
        else
            inst[ "replaceWith" ]( rpmt );
    }

    void replaceWith( DomInterface rpmt )
    {   replaceWith( rpmt.inst );   }

    val x_( string xpath ) 
    {   if( inst == js_null())   return js_null();
        return inst.call< val >( "xpath", xpath ); 
    }

    string x_s( string xpath ) 
    {   if( inst == js_null())   return s( "" );
        val res = inst.call< val >( "xpath", xpath );
        if( res == js_null())   return s( "" );
        return res.call< string >( "toString" );
    }

    string properText() { return s( x_( "text()" )[ "data" ]); }

    PropProxy operator[]( const string& key )
    {   return PropProxy{ inst, key };   }

    string get( const string& key )
    {
        PropProxy prop = PropProxy{ inst, key };
        val res = prop.resolve();

        if( res == js_null())   return "";
        string is_string = Object[ "prototype" ][ "toString" ].call< string >( "call", res );
        if( is_string.find( "tring]" ) != string::npos )
            return res.as< string >();
        if( g_[ "Reflect" ].call< val >( "has", res, s( "toString" )).as< bool >())
            return res.call< string >( "toString" );
        return is_string;
    }

    string get( const string& key, const string default_ )
    {
        PropProxy prop = PropProxy{ inst, key };
        val res = prop.resolve();

        if( res == js_null())   return default_;
        return get( key );
    }
};


void multi_set( DomInterface elem_, std::map< string, string > props )
{
    // 【 context helper 】
    for( auto prop : props )
        elem_[ prop.first.c_str()] = prop.second;
}


val elem( std::string_view selector )
{
    val res = js_null();

    try
    {   res = g_.call< val >( "elem", s( selector ));
    }catch( ... )
    {}
    if( res == js_null())
        res = g_.call< val >( "xpath_", s( selector ));
    return res;
}


bool meta_debug = true;

string meta_d_include = R"-(||)-";

string meta_d_exclude = "";


val d_get_( val variable, string title, string func_ )
{
    for( bool run_once = false;
            meta_debug && meta_d_exclude.find( s( "|" ) + func_ + s( "|" )) == string::npos && !run_once;
            run_once = true )
    {
        if( meta_d_include != s( "" ) && meta_d_include.find( s( "|" ) + func_ + s( "|" )) == string::npos )
            break;
        console[ "log" ]( s( "in " ) + func_ + s( " : " ) + title );
        console[ "dir" ]( variable );

        string serial = "null";

        if( variable != js_null())
        {
            serial = g_.call< string >( "to_JSON_string", variable );
            if( serial == "false" || serial == "{}" )
            {
                string is_object = Object[ "prototype" ][ "toString" ].call< string >( "call", variable );

                if( is_object.find( "lement]" ) != string::npos )
                {   serial = s( variable[ "outerHTML" ]);
                }
                else
                {   if( g_[ "Reflect" ].call< val >( "has", variable, s( "toString" )).as< bool >())
                    {   serial = variable.call< string >( "toString" );
                    }
                    else
                        serial = is_object;
                }
            }
        }

        console[ "log" ]( serial );
    }
    return variable;
}


string d_get_( string variable, string title, string func_ )
{
    if( meta_debug && meta_d_exclude.find( s( "|" ) + func_ + s( "|" )) == string::npos )
        if( meta_d_include == s( "" ) || meta_d_include.find( s( "|" ) + func_ + s( "|" )) != string::npos )
        {   console[ "log" ]( s( "in " ) + func_ + s( " : " ) + title );
            console[ "dir" ]( variable );
        }

    return variable;
}


#define d_get( var, title )  d_get_( var, title, __func__ )


val clone( val elem_ )
{
    return elem_.call< val >( "cloneNode", true );
}


// mapa < termo mesis, vec< tokens: =tagname, .classes, #id > >
// termo eh _nnnnn__[tag][.classe1[ ... .classen][ ][#id]
std::map< string, std::vector< string > > ref_term_tokens;

// lista dos termos (termo em Mesis = tag.classes#id) q sao referencias
// (podem ser declaradas fora da hierarquia normal do HTML). Esses termos estarao
// no mapa ref_term_tokens, e vem do obj JS __mesis_ref_term_tokens (sao suas keys)
std::vector< string > ref_terms_list;


// config: acrescenta cache-bursts
bool no_cache = false;


// insere referencia sem outras refs internas (leafs) na posicao correta da hierarquia html
void inject_leafs( val tgt_subtree, std::map< string, bool > leafs )
{
    // se a subtree fornecida eh undef, nada a fazer
    if( tgt_subtree == js_null())
        return;

    DomInterface subtree = { tgt_subtree };

    // TODO: esta funcionando, mas tvz seja mais adequado usar obj.proto.toString.call ? ou typeOf?
    string _typeof = tgt_subtree.call< string >( "toString" );

    // nao tem children (na realidade, nao eh um objeto, entao nao tem keys)
    if( _typeof != string( "[object Object]" ))
    {
        // verifica se eh uma array TODO: inserir um typeOf na DomInterface
        if( tgt_subtree[ "fill" ] != js_null() && tgt_subtree[ "every" ] != js_null())
        {
            // nesse caso temos q chamar inject_leaf para cada elemento, se
            // os elementos em si forem objetos ou arrays
            int length = tgt_subtree[ "length" ].as< int >();

            for( int i = 0; i < length; i++ )
                inject_leafs( tgt_subtree[ i ], leafs );
        }
        return;
    }

    std::vector< string > children = subtree.keys();

    for( string child_name : children )
    {
        bool match = false;

        string child_tag = "div";

        // se nao existe menção no mapa para o elemento, ele eh uma declaracao de tag
        // direta
        if( ref_term_tokens.find( child_name ) == ref_term_tokens.end())
            child_tag = child_name;

        // identificando tagname e id da child em teste (se existem)
        for( string child_part : ref_term_tokens[ child_name ])
            if( child_part[ 0 ] == '=' )
                if( child_part.substr( 1 ) != "" )
                {
                    child_tag = child_part.substr( 1 );
                    break;
                }

        string child_id = "";

        for( string child_part : ref_term_tokens[ child_name ])
            if( child_part[ 0 ] == '#' )
                if( child_part.substr( 1 ) != "" )
                {
                    child_id = child_part.substr( 1 );
                    break;
                }

        for( auto& leaf : leafs )
        {
            // child name vem da tgt_subtree, leafs vem dos globais (refs)
            if( child_name == leaf.first )
                match = true;
            if( !match )
            {
                // testa se uma ref mais abrangente da match
                bool have_tag = false;
                string leaf_tag = "";
                std::vector< string >& leaf_term_tokens = ref_term_tokens[ leaf.first ];

                for( string leaf_part : leaf_term_tokens )
                {
                    if( leaf_part[ 0 ] == '=' )
                    {
                        have_tag = true;
                        leaf_tag = leaf_part.substr( 1 );
                        break;
                    }
                }

                if( have_tag && child_tag == leaf_tag )
                    match = true;

                if( have_tag && !match )
                    continue;

                bool have_id = false;
                string leaf_id = "";
                for( string leaf_part : leaf_term_tokens )
                    if( leaf_part[ 0 ] == '#' )
                    {
                        have_id = true;
                        leaf_id = leaf_part.substr( 1 );
                        break;
                    }

                if( have_id && child_id == leaf_id )
                    match = true;

                if( have_id && !match )
                    continue;

                int classes_found = 0;
                int classes_tested = 0;

                for( string leaf_part : leaf_term_tokens )
                    if( leaf_part[ 0 ] == '.' )
                    {
                        classes_tested++;

                        for( string child_part : ref_term_tokens[ child_name ])
                            if( child_part[ 0 ] == '.' )
                                if( leaf_part == child_part )
                                {
                                    classes_found++;
                                    break;
                                }
                    }
                if( classes_tested > classes_found && classes_tested > 0 )
                {
                    match = false;
                    continue;
                }
                // aqui match ja deve ser true de qq forma
                match = true;
            }

            if( match )
            {
                val deep = window.call< val >( "structuredClone", html_src_v[ leaf.first ]);

                // se for um texto, temos q criar um objeto com textContent, se
                // for uma array, sera um push nao um assign
                string _typeof_ = tgt_subtree[ child_name ].call< string >( "toString" );

                if( _typeof_ != "[object Object]" )
                {
                    // verifica se eh uma array
                    if( tgt_subtree[ child_name ][ "fill" ] != js_null()
                            && tgt_subtree[ child_name ][ "every" ] != js_null())
                    {
                        tgt_subtree[ child_name ].call< void >( "push", deep );
                        break;
                    }
                    else
                    {
                        val encapsulate = Object.new_();
                        encapsulate.set( "textContent", tgt_subtree[ child_name ]);
                        tgt_subtree.set( child_name, encapsulate );
                    }
                }
                Object.call< void >( "assign", tgt_subtree[ child_name ], deep );
                break;
            }
        }

        if( !match )
            // se ocorreu injecao, nao testa os children, pq como sempre injetamos
            // leafs (q nao tem referencias internas), isso nao eh necessario. Mas
            // se nao tivemos match, desce na  arvore
            inject_leafs( tgt_subtree[ child_name ], leafs );
    }
}


// guarda as configuracoes do Mesis
std::map< string, string > gConfig_store = {};


void _erase_mesis_object()
{
    // apaga o objeto Mesis, exceto os objetos marcados com a decoracao "keep"
    // (e o html, q eh sempre removido)
    for( auto it = ref_term_tokens.begin(); it != ref_term_tokens.end(); /* empty */ )
    {
        string term = it->first;

        string is_html_tag = "";
        if( term.find( "__" ) != string::npos )
            is_html_tag = term.substr( term.find( "__" ));

        if( html_src_v[ "decoration" ][ term ] == js_null() || is_html_tag == "__html" )
        {
            it = ref_term_tokens.erase( it );
            html_src_v.call< void >( "delete", term );
            global_v[ "__mesis_ref_term_tokens" ].call< void >( "delete", term );

            auto it2 = std::remove( ref_terms_list.begin(), ref_terms_list.end(), term );
            ref_terms_list.erase( it2, ref_terms_list.end());
        }
        else ++it;
    }
}


val __copy_script( val src_script )
{
    val new_script = doc.call< val >( "createElement", string( "script" ));

    val attrs = src_script[ "attributes" ];
    uint32_t lim = attrs[ "length" ].as< uint32_t >();

    for( uint32_t c = 0; c < lim; c++ )
    {
        val attr = attrs[ c ];
        new_script.call< void >( "setAttribute", s( attr[ "name" ]), s( attr[ "value" ]));
    }

    new_script.set( "textContent", s( src_script[ "textContent" ]));

    return new_script;
}


void __copy_script( val src_script, val target )
{
    val new_script = __copy_script( src_script );
    target.call< void >( "appendChild", new_script );
}



val __copy_script( string script_outerHTML )
{
    val parser = val::global( "DOMParser" ).new_();

    string model_html = s( "<html><head>" ) + script_outerHTML + s( "</head></html>" );

    val script = parser.call< val >
    ( "parseFromString", model_html, string( "text/html" ));

    script = script[ "documentElement" ][ "children" ][ 0 ][ "children" ][ 0 ];

    val attrs = script[ "attributes" ];
    uint32_t lim = attrs[ "length" ].as< uint32_t >();

    val new_script = doc.call< val >( "createElement", string( "script" ));

    for( uint32_t c = 0; c < lim; c++ )
    {
        val attr = attrs[ c ];
        new_script.call< void >( "setAttribute", s( attr[ "name" ]), s( attr[ "value" ]));
    }

    new_script.set( "textContent", s( script[ "textContent" ]));

    return new_script;
}


// @note pega o objeto global JS__mesis_ref_term_tokens q contem as tags = parsed(tags)
// e retorna um vetor com as chaves (lista de termos => eu chamo o definidor da tag de 'termo')
std::vector< string > _get_terms_list()
{
    DomInterface ref_term_tokens_v = { g_[ "__mesis_ref_term_tokens" ]};
    //std::vector< string > ref_terms_list = _val_to_keys( ref_term_tokens_v );
    auto ref_terms_list = ref_term_tokens_v.keys();

    // remove a funcao delete da lista
    auto it = std::remove( ref_terms_list.begin(), ref_terms_list.end(), "delete" );
    ref_terms_list.erase( it, ref_terms_list.end());

    // tb reconstroi o mapa geral no lado do c++ antes de continuar
    ref_term_tokens.clear();
    for( string term : ref_terms_list )
    {
        ref_term_tokens[ term ]
            = split( s( g_[ "__mesis_ref_term_tokens" ][ term ]), "<>" );
    }

    return ref_terms_list;
}


// auxiliar: os prefixos internos do Mesis sao na forma __nnnnn__seletor
inline string __strip_prefix( string name )
{
    string raw_name = name;

    if( name[ 0 ] == '_' )
        if( name.find( "__" ) != string::npos )
        {
            string prefix = name.substr( 1, name.find( "__" ) - 1 );
            if( std::all_of( prefix.begin(), prefix.end(),
                []( char c )
                {   return ::isdigit( static_cast< unsigned char >( c )); }
            ))
                raw_name = name.substr( name.find( "__" ) + 2 );
        }

    return raw_name;
}


// leaf = no sem referencias internas
bool check_node_is_leaf( val def_subtree )
{
    // se nao eh uma definicao de uma referencia, retorna falso. Aqui
    // funciona pq a funcao chamante esta procurando leafs, mas o ideal
    // seria responder em tri-state
    if( def_subtree == js_null())
        return false;

    DomInterface subtree = { def_subtree };
    string _typeof = def_subtree.call< string >( "toString" );

    // nao tem children (na realidade, nao eh um objeto, entao nao tem keys)
    if( _typeof != string( "[object Object]" ))
    {
        // verifica se eh uma array
        if( def_subtree[ "fill" ] != js_null()
                && def_subtree[ "every" ] != js_null())
        {
            // nesse caso temos q chamar check_node_is_leaf para cada elemento, se
            // os elementos em si forem objetos ou arrays
            int length = def_subtree[ "length" ].as< int >();

            for( int i = 0; i < length; i++ )
                if( check_node_is_leaf( def_subtree[ i ]) == false )
                    return false;
            return true;
        }
    }

    //std::vector< string > children = _val_to_keys( def_subtree );
    auto children = subtree.keys();

    for( string child_name : children )
    {
        if( def_subtree[ child_name ] == js_null()) continue;

        // se o seletor aparece no mapa q foi criado pela Underscore sem
        // parametros, e se temos
        // um objeto global declarando uma referencia com o seletor,
        // ja sabemos q o parent nao eh uma folha
        if( ref_term_tokens.find( child_name ) != ref_term_tokens.end()
                && html_src_v[ child_name ].as< bool >())
            return false;

        // no entanto se NAO temos o objeto global com a referecia exatam/e igual
        // ao seletor em analise ainda nao podemos afirmar
        // q nao eh uma folha, pq existem casos de declarar um elemento com
        // tag.class #id e ter uma regra #id ou .class ou tag.class (mas nao tag.id)
        // q se aplicara. Ja regras tag.classe1.classe2 sao sucetiveis a referencias
        // .classe1.classe2, tag.classe1, tag.classe2, .classe2.classe1, etc.

        // temos entao q checar o mapa inteiro contra esse seletor

        // partes de uma referencia nao implicam em tag "div" por padrao
        // (para poder existir referencias a classes ou id puros), mas
        // partes de um seletor comum numa subtree html implicam em div
        // por padrao (pq todo seletor numa subtree eh traduzido para uma tag)
        string child_tag = "div";

        // se nao existe menção no mapa para o elemento, ele eh uma declaracao de tag
        // direta
        if( ref_term_tokens.find( child_name ) == ref_term_tokens.end())
            child_tag = child_name;

        for( string child_part : ref_term_tokens[ child_name ])
            if( child_part[ 0 ] == '=' )
                if( child_part.substr( 1 ) != "" )
                {
                    child_tag = child_part.substr( 1 );
                    break;
                }

        string child_id = "";

        for( string child_part : ref_term_tokens[ child_name ])
            if( child_part[ 0 ] == '#' )
                if( child_part.substr( 1 ) != "" )
                {
                    child_id = child_part.substr( 1 );
                    break;
                }

        for( auto& ref_pair : ref_term_tokens )
        {
            string ref_selec = ref_pair.first;

            if( ref_selec == child_name )
                // nao verifica contra si mesmo
                continue;

            // pra começar, seletores q nao tem blocos de referencia definidos
            // nao precisam ser testados aqui
            if( html_src_v[ ref_selec ] == js_null()) continue;

            std::vector< string >& elem_term_tokens = ref_pair.second;

            // nesta varredura geral, se todos os componentes deste outro seletor
            // sao match contra o seletor especifico em teste, temos uma ref
            // valida (e porntanto esse no nao eh uma folha)
                // testa tag se E
            bool have_tag = false;
            string ref_tag = "";
            for( string ref_part : elem_term_tokens )
            {
                if( ref_part[ 0 ] == '=' )
                {
                    have_tag = true;
                    ref_tag = ref_part.substr( 1 );
                    break;
                }
            }

            if( have_tag && ref_tag != child_tag )
                // tag no seletor nao bate com esta, encerra o teste desse seletor
                continue;

            // mesmo teste para ids (so pode existir um unico componente id)
            bool have_id = false;
            string ref_id = "";
            for( string ref_part : elem_term_tokens )
                if( ref_part[ 0 ] == '#' )
                {
                    have_id = true;
                    ref_id = ref_part.substr( 1 );
                    break;
                }

            if( have_id && ref_id != child_id )
                // id do seletor nao bate com este id, encerra o teste para este seletor
                continue;

            // quanto a classes, todas as da referencia tem q estar
            // no seletor - o seletor pode ter mais e ainda sera um
            // match valido, mas se qq uma das classes da ref nao
            // estiver no seletor, nao eh uma referencia para ele
            int classes_found = 0;
            int classes_tested = 0;

            for( string ref_part : elem_term_tokens )
                if( ref_part[ 0 ] == '.' )
                {
                    classes_tested++;

                    for( string child_part : ref_term_tokens[ child_name ])
                        if( child_part[ 0 ] == '.' )
                            if( ref_part == child_part )
                            {
                                classes_found++;
                                break;
                            }
                }
            if( classes_tested > classes_found || classes_tested == 0 ) continue;
        }

        // desce na arvore
        string _typeof = def_subtree[ child_name ].call< string >( "toString" );

        if( _typeof == string( "[object Object]" ))
            if( !check_node_is_leaf( def_subtree[ child_name ]))
                return false;
    }

    return true;
}


val _mesis_rebuild_html_hierarchy()
{
    // local, map< seletor, bool >
    std::map< string, bool > leafs;

    // uma lista de exclusao, se as leafs nao forem referenciadas elas podem
    // estar sendo usadas por scripts, mas se nao fossem excluidas do algoritmo,
    // seriam selecionadas todas as vezes e teriamos um loop infinito
    std::map< string, bool > leafs_excusion;

    while( true )
    {
        leafs.clear();

        // procura por leafs (objetos Mesis sem referencias a outros)
        for( string selector : ref_terms_list )
        {
            if( html_src_v[ selector ].as< bool >() == false ) continue;

            if( __strip_prefix( selector ) == "html" ) continue;

            // itera por esse objeto e verifica se ele contem qq referencia. Se sim,
            // nao eh um node terminal (folha).
            val ref_subtree = html_src_v[ selector ];
            bool is_leaf = check_node_is_leaf( ref_subtree );

            if( is_leaf && leafs_excusion.find( selector ) == leafs_excusion.end())
                leafs[ selector ] = true;
        }

        // se nao temos mais folhas, acabamos esta fase
        if( leafs.size() == 0 )
            //no more leafs, ascending in the tree"
            break;

        // agora procura nos demais nos e em html referencias às folhas e as
        // injeta (injetar = inserir na posicao correta da hierarquia)
        for( string selector : ref_terms_list )
        {
            // nao tenta injetar as leafs nelas mesmas (excesso de zelo, ja q
            // elas nao tem referencias injetaveis)
            for( auto& leaf : leafs )
            {
                if( selector == leaf.first )
                    continue;
            }

            if( html_src_v[ selector ].as< bool >() == false ) continue;
            if( leafs.find( selector ) != leafs.end()) continue;

            val tgt_subtree = html_src_v[ selector ];
            inject_leafs( tgt_subtree, leafs );
        }

        // acrescenta as leafs usadas ou no mapa de exclusao
        for( auto& leaf : leafs )
            leafs_excusion[ leaf.first ] = true;
    }

    val html_v = js_null();

    for( string key : ref_terms_list )
    {
        string is_html_tag = key.substr( key.find( "__" ));
        if( is_html_tag == "__html" )
        {   html_v = html_src_v[ key ];
            break;
        }
    }

    return html_v;
}


string mesis_to_html( string selector, val mesis_object )
{
    selector = trim( selector );

    val mobj = mesis_object;

    // se nao eh um objecto Mesis valido, retorna string vazia.
    if( mobj == js_null())
        return "";

    DomInterface mobj_di = { mobj };

    string gen_htm = "";
    string _typeof = mobj.call< string >( "toString" );

    string tagname = "div";
    bool remove_map_entry = false;

    if( selector != "html" )
    {
        gen_htm = "<";
        std::vector< string > css_classes;

        // verifica no mapa se existe referencia e usa as partes ja separadas,
        // senao temos q fazer o parse da tag
        if( ref_term_tokens.find( selector ) == ref_term_tokens.end())
        {
            global_v.call< void >( "mesis_parse_tag", selector );
            remove_map_entry = true;

            if( global_v[ "__mesis_ref_term_tokens" ][ selector ] != js_null())
                ref_term_tokens[ selector ]
                    = split( s( global_v[ "__mesis_ref_term_tokens" ][ selector ]), "<>" );
        }

        if( ref_term_tokens.find( selector ) != ref_term_tokens.end()
                && ref_term_tokens[ selector ].size() > 0 )
        {
            std::vector< string > parts = ref_term_tokens[ selector ];

            bool tag = false;
            for( string part : parts )
                if( part[ 0 ] == '=' )
                {   tag = true;
                    tagname = part.substr( 1 );
                    gen_htm += tagname;
                    break;
                }
            if( tag == false )
                gen_htm += tagname; // deve ser div por default

            for( string part : parts )
                if( part[ 0 ] == '#' )
                {   gen_htm += " id=\"" + part.substr( 1 ) + "\"";
                    break;
                }

            // acumula classes pq elas podem ter sido declaradas parte
            // no seletor, parte num atributo attrs dentro do objeto
            for( string part : parts )
                if( part[ 0 ] == '.' )
                    css_classes.push_back( part.substr( 1 ));
        }
        else
        {
            tagname = __strip_prefix( selector );
            gen_htm += tagname;
        }

        // verifica se ha um ou mais atributos com "attrs" no inicio como children
        if( _typeof == string( "[object Object]" ))
        {
            //std::vector< string > children = _val_to_keys( mobj );
            auto children = mobj_di.keys();

            for( string child_name : children )
            {
                string raw_name = __strip_prefix( child_name );

                if( raw_name.substr( 0, 5 ) == "attrs" )
                {
                    // acumula se for classe css, ou acrescenta na tag ja se nao for
                    val attrs_v = mobj[ child_name ];
                    DomInterface attrs_di = { attrs_v };

                    //std::vector< string > attrs = _val_to_keys( attrs_v );
                    auto attrs = attrs_di.keys();

                    for( string attr_name : attrs )
                    {
                        string raw_attr_name = __strip_prefix( attr_name );

                        if( raw_attr_name == "class" )
                        {
                            val attr_o = attrs_v[ attr_name ];

                            // se for uma array, constroi a string
                            if( attr_o[ "fill" ] != js_null()
                                    && attr_o[ "every" ] != js_null())
                            {
                                int len = attr_o[ "length" ].as< int >();
                                for( int i = 0; i < len; i ++ )
                                    css_classes.push_back( s( attr_o[ i ]));
                            }
                            else
                                css_classes.push_back( s( attr_o ));
                        }
                        // se nao for class, adiciona imediatamente na tag
                        else
                        {
                            val attr_o = attrs_v[ attr_name ];
                            gen_htm += " " + raw_attr_name + "=\"";
                            if( attr_o[ "fill" ] != js_null()
                                    && attr_o[ "every" ] != js_null())
                            {
                                int len = attr_o[ "length" ].as< int >();
                                for( int i = 0; i < len; i ++ )
                                    gen_htm += s( attr_o[ i ]) + " ";
                            }
                            else
                                gen_htm += s( attr_o );
                            gen_htm += "\"";
                        }
                    }
                }
            }
        }

        // se achamos classes css, totaliza aqui
        if( css_classes.size() > 0 )
        {
            gen_htm += " class=\"";
            for( string class_ : css_classes )
                gen_htm += class_ + " ";
            gen_htm += "\"";
        }

        gen_htm += ">";

        // verifica se ha um ou mais atributos com "textContent" no inicio como children
        if( _typeof == string( "[object Object]" ))
        {
            //std::vector< string > children = _val_to_keys( mobj );
            auto children = mobj_di.keys();

            for( string child_name : children )
            {
                string raw_name = __strip_prefix( child_name );

                if( raw_name.substr( 0, 11 ) == "textContent" )
                    gen_htm += mobj[ child_name ].call< string >( "toString" );
            }
        }
    }

    // agora desce pela arvore
    if( _typeof == string( "[object Object]" ))
    {
        //std::vector< string > children = _val_to_keys( mobj );
        auto children = mobj_di.keys();

        for( string child_name : children )
        {
            string raw_name = __strip_prefix( child_name );

            if( raw_name.substr( 0, 5 ) != "attrs" && raw_name.substr( 0, 11 ) != "textContent" )
                gen_htm += mesis_to_html( child_name, mobj[ child_name ]);
        }
    }
    else
    {
        // verifica se eh uma array
        if( mobj[ "fill" ] != js_null() && mobj[ "every" ] != js_null())
        {
            int length = mobj[ "length" ].as< int >();

            for( int i = 0; i < length; i++ )
                gen_htm += mesis_to_html( __strip_prefix( selector ), mobj[ i ]);
        }
        else
            // eh um literal
            gen_htm += _typeof;
    }

    if( remove_map_entry == true )
        ref_term_tokens.erase( selector );

    // finalmente fecha a tag
    if( selector != "html" )
        gen_htm += "</" + tagname + ">";

    return gen_htm;
}



void kv_add( val kv_Map )
{
    val keys = kv_Map.call< val >( "keys" );
    int len_ = kv_Map[ "size" ].as< int >();

    for( uint32_t c = 0; c < len_; c++ )
    {
        string key = keys.call< val >( "next" )[ "value" ].call< string >( "toString" );
        kv_store[ key ] = val( kv_Map.call< val >( "get", key ));
    }
}


void kv_add_jo( val kv_pojo )
{
    val keys = Object.call< val >( "keys", kv_pojo );
    int len_ = keys[ "length" ].as< int >();

    for( uint32_t c = 0; c < len_; c++ )
    {
        string key = s( keys[ c ]);
        kv_store[ key ] = val( kv_pojo[ key ]);
    }
}


val kv_get( string key )
{
    if( kv_store.find( key ) != kv_store.end())
        return kv_store[ key ];
    else
        return js_null();
}


bool kv_del( string key )
{
    if( kv_store.find( key ) != kv_store.end())
    {
        kv_store.erase( key );
        return true;
    }
    else
        return false;
}


val kv_search( string query )
{
    val res = g_[ "Map" ].new_();

    for( const auto& [key, value] : kv_store )
    {
        if( key.find( query ) != string::npos )
            res.call< void >( "set", key, value );
    }

    return res;
}


val kv_remove( string key )
{
    val res = js_null();

    if( kv_store.find( key ) != kv_store.end())
    {
        res = kv_store[ key ];
        kv_del( key );
        return res;
    }
    else
        return js_null();
}


void loop( val function_, int16_t frame_window, int16_t frame_pos, val params_Map )
{
    // frame window < 0 == conte tantos frames (intervalo)
    if( frame_window < 0 )
        frame_pos = frame_window * -1;
    else
        // no regime positivo, frame_window eh uma janela de frames de execucao circular,
        // e no momento do registro podemos estar em qq parte do intervalo. Nesse caso,
        // posicao do frame >= tamanho da janela eh incorreto.
            // no negativo frame_window é uma quantidade de frames exatas a contar antes
        // de rodar, e frame_pos eh ignorado
        if( frame_pos >= frame_window ) frame_pos = 0;

    string fname = s( function_[ "name" ]);
    on_animation_frame[ fname ] = { frame_window, frame_pos };
    kv_store[ fname ] = function_;

    if( params_Map != js_null())
    {
        string param_string_Map = "";

        val keys = params_Map.call< val >( "keys" );
        int len_ = params_Map[ "size" ].as< int >();

        for( uint32_t c = 0; c < len_; c++ )
        {
            string key = keys.call< val >( "next" )[ "value" ].call< string >( "toString" );

            if( key.find( "meta_flow" ) == string::npos )
                param_string_Map += key + "::";

            kv_store[ fname + s( "::" ) + key ] = params_Map.call< val >( "get", key );
        }

        param_string_Map = param_string_Map.substr( 0, param_string_Map.size() - 2 );
        kv_store[ fname + s( "::params" )] = val( param_string_Map );
    }
}


// para casos como nome = funtion outro_nome()
void loop_s( string fname, val function_, int16_t frame_window, int16_t frame_pos, val params_Map)
{
    // frame window < 0 == conte tantos frames (intervalo)
    if( frame_window < 0 )
        frame_pos = frame_window * -1;
    else
        // no regime positivo, frame_window eh uma janela de frames de execucao circular,
        // e no momento do registro podemos estar em qq parte do intervalo. Nesse caso,
        // posicao do frame >= tamanho da janela eh incorreto
        if( frame_pos >= frame_window ) frame_pos = 0;

    on_animation_frame[ fname ] = { frame_window, frame_pos };
    kv_store[ fname ] = function_;

    if( params_Map != js_null())
    {
        string param_string_Map = "";

        val keys = params_Map.call< val >( "keys" );
        int len_ = params_Map[ "size" ].as< int >();

        for( uint32_t c = 0; c < len_; c++ )
        {
            string key = keys.call< val >( "next" )[ "value" ].call< string >( "toString" );

            if( key.find( "meta_flow" ) == string::npos )
                param_string_Map += key + "::";
            kv_store[ fname + s( "::" ) + key ] = params_Map.call< val >( "get", key );
        }

        param_string_Map = param_string_Map.substr( 0, param_string_Map.size() - 2 );
        kv_store[ fname + s( "::params" )] = val( param_string_Map );
    }
}


void Mesis_remove_from_loop( string global_function )
{
    if( on_animation_frame.find( global_function ) != on_animation_frame.end())
        on_animation_frame.erase( global_function );

    // parameters E? erase them
    if( kv_store.find( global_function ) != kv_store.end())
    {
        val params = kv_search( global_function );
        val keys = params.call< val >( "keys" );
        uint32_t len_ = params[ "size" ].as< uint32_t >();

        for( uint32_t c = 0; c < len_; c++ )
        {
            string key = keys.call< val >( "next" )[ "value" ].call< string >( "toString" );
            kv_del( key );
        }
    }
}


void loop_off( val function_ )
{
    string fname = s( function_[ "name" ]);
    Mesis_remove_from_loop( fname );
}


void loop_off_s( string function_name )
{
    Mesis_remove_from_loop( function_name );
}


void loop_dump()
{
    for( auto item: on_animation_frame )
        console[ "log" ]( item.first );
}


void Mesis_pause_on_loop( string global_function )
{
    if( on_animation_frame.find( global_function ) != on_animation_frame.end())
    {
        std::pair< int16_t, int16_t > exec_cycle = on_animation_frame[ global_function ];
        if( exec_cycle.first > 0 )
            on_animation_frame[ global_function ]
                = { exec_cycle.first, exec_cycle.second == 0
                        ? -1 : abs( exec_cycle.second + 1 ) * -1 }; // reversivel, -1 == 0
    }
}


void Mesis_restart_on_loop( string global_function )
{
    if( on_animation_frame.find( global_function ) != on_animation_frame.end())
    {
        std::pair< int16_t, int16_t > exec_cycle = on_animation_frame[ global_function ];
        int16_t frame_pos = exec_cycle.second;
        if( frame_pos < 0 )
        {   frame_pos = ( frame_pos * -1 ) - 1;
            on_animation_frame[ global_function ] = { exec_cycle.first, frame_pos };
        }
        if( exec_cycle.first == 0 )
            on_animation_frame[ global_function ] = { 1, frame_pos };
    }
    else
        std::cout << "Mesis_restart_on_loop called, but the function " << global_function
                    << " was not found. Use Mesis_add_to_loop" << std::endl;
}



bool loop_stoped = false;

// TODO: DEBUG: nao expor apos o DEBUG
void stop_loop()
{
    loop_stoped = true;
}


// fila de islands - a fila existe
// pq podemos ter chamadas em paralelo mas a execucao deve ser sincrona.
// deque para preservar a ordem de chamada
std::deque< string > island_addrs = {};
std::map< string, string > islands_store = {};

std::map< string, bool > islands_no_cache = {};

val cacheAPI = js_null();


void fetch_island( string url, string selector_tgt )
{
    // o loop interno do Mesis lida com o processo, aqui somente
    // jogamos a chamada na fila

    if( islands_no_cache.find( url ) != islands_no_cache.end())
        islands_no_cache[ "url" ] = false;
    island_addrs.push_back( url + s( "::" ) + selector_tgt );
}


void fetch_island_no_cache( string url, string selector_tgt )
{
    // fetch_island above will cache and get islands from the cache, this fn will not
    islands_no_cache[ "url" ] = true;
    fetch_island( url, selector_tgt );
}


void island_response( string url, string selector, val response, bool error )
{
    // response is not  a real response object here, it is a POJO
    auto& no_cache = islands_no_cache;

    if( error == false )
    {
        // if response.text is empty, see if we have the text in the cache API
        if( s( response[ "text" ]).size() == 0 )
        {
                // if it was not requested as no-cache
            if( no_cache.find( url ) == no_cache.end() || no_cache[ url ] == false )
            {   string island_downloaded_entry = s( "island_downloaded::" ) + url;
                    // if it was downloaded before
                if( kv_store.find( island_downloaded_entry ) != kv_store.end())
                {   g_.set( "ffacet_cache", cacheAPI );
                    EM_ASM(
                    {   const url = UTF8ToString( $0 ).trim();
                        const selector = UTF8ToString( $1 ).trim();

                        globalThis[ "ffacet_cache" ].match( url )
                            .then( response => response.text())
                            .then( text =>
                            {   // cant send empty texts here or we enter an infinite loop
                                if( text == "" )
                                    text = "<html></html>";
                                MesisAgent.island_response( url, selector, { text: text }, false );
                            });
                            // TODO: catch aqui? qdo a cache API levanta excecao?
                    },
                    url.c_str(), selector.c_str());
                    return;
                }
            }
        }

        val regexp = g_[ "RegExp" ].new_( s( R"-([\s]*Mesis[\s]*\[[\s]*_[\s]*`html)-" ));
        if( regexp.call< bool >( "test", response[ "text" ]) == false )
        {
            // if there is no Mesis script, we check if we have html generated in the past
            // (in the cache)
                // if it was not requested as no-cache
            if( no_cache.find( url ) == no_cache.end() || no_cache[ url ] == false )
            {   string island_downloaded_entry = s( "island_downloaded::" ) + url;
                    // if it was downloaded before
                if( kv_store.find( island_downloaded_entry ) != kv_store.end())
                {
                    islands_store[ url + s( "::" ) + selector ] = s( response[ "text" ]);
                    return;
                }
            }
            // otherwise it is an error
            char btick = static_cast< char >( 96 );
            std::cerr << "Mesis (in island) didn't find the HTML generation script - nothing will be generated. "
                    << "Please add a SCRIPT with Mesis[_" << btick << "html" << btick
                    << "] = ... declaration" << std::endl;

            string error_html = s( "Empty Island</h1><br/><span>" ) + url
                                + s( "</span><br/><br/><span>main JS not found</span></div>" );

            // if we have ext| in the selector, it indicates a dettached doc frag,
            // so in this case we don't inject an error msg
            if( selector.find( "ext|" ) == string::npos )
            {   val tgt = elem( selector );
                tgt.set( "innerHTML", error_html );
            }
            return;
        }

        islands_store[ url + s( "::" ) + selector ] = s( response[ "text" ]);
        return;
    }

    if( error )
        islands_store[ url + s( "::" ) + selector ] = s( response[ "message" ]);
}


// implements head tags removal when they have a data-dependency parameter.
// This fn is exposed to be used if we avoid fetch_island (if we cached
// the island for instance). This fn also implements adding tags to the head
// (if it receives a block of tags to be added to the head it uses Mesis
// addition algorithm that avoid duplications in the head)
    // if we call curate_head( url, null ) it runs the cleanup algorithm,
// if we call with a valid nodes param, the cleanup algorithm do not run and
// nodes are added to head
void curate_head( string island_url, val nodes, val cache_el )
{
    DomInterface head = { doc[ "head" ]};
    string head_innerHTML = "";

    if( island_url.size() > 0 && nodes == js_null())
    {   val del_cands = head.x_(  "[]://style | //link[@rel='stylesheet'] | //link[@rel='preconnect'] | //script" );
        uint32_t lim = del_cands[ "length" ].as< uint32_t >();

        val del_elems = g_[ "Array" ].new_();

        for( int32_t c = 0; c < lim; c++ )
        {
            DomInterface should_del = { del_cands[ c ]};

            if( should_del[ "data-dependency" ] != js_null())
                if( island_url.find( s( should_del[ "data-dependency" ])) == string::npos
                        && s( should_del[ "data-dependency" ]).find( island_url ) == string::npos )
                    del_elems.call< void >( "unshift", should_del.inst );
        }

        lim = del_elems[ "length" ].as< uint32_t >();

        for( int32_t c = 0; c < lim; c++ )
        {   val to_del = del_elems[ c ];
            to_del.call< void >( "remove" );
        }
        return;
    }

    string is_string = Object[ "prototype" ][ "toString" ].call< string >( "call", nodes );

    if( is_string.find( "ull]" ) != string::npos || is_string.find( "ndefined]" ) != string::npos )
        return;

    // if we provide nodes as string, we convert it to elements
    val elems_ = nodes;
    if( is_string.find( "tring]" ) != string::npos )
    {
        val parser = g_[ "DOMParser" ].new_();
        val doc_frag = parser.call< val >( "parseFromString", nodes, string( "text/html" ));

        elems_ = doc_frag[ "body" ][ "children" ];
    }
    else
    {
        // if we provided nodes as non-iterable, wrap it in an array
        if( is_string.find( "ollection]" ) == string::npos
                && is_string.find( "rray]" ) == string::npos )
        {   elems_ = g_[ "Array" ].new_();
            elems_.call< void >( "push", nodes );
        }
    }

    int lim = elems_[ "length" ].as< int >();
    for( int c = 0; c < lim; c++ )
    {
        // first attempt to avoid duplication is plain-text if the element is small
        string elem_html = s( elems_[ c ][ "outerHTML" ]);
        if( elem_html.size() < 300 )
        {
            if( head_innerHTML.size() == 0 )   head_innerHTML = s( head[ "innerHTML" ]);
            if( head_innerHTML.find( elem_html ) != string::npos )
            {   d_get( s( "already E: elem w/ identical outerHTML " ) + elem_html, s( "skipped" ));
                continue;
            }
        }

        // then we test for a data-mesis_id attribute, and this way an element is injected only
        // once even if it appears across multiple files - this is one not intended to be temporary
        // (using data-dependency is)
        DomInterface elem_ = { elems_[ c ]};
        string mesis_id = elem_.get( "data-mesis_id" );
        if( mesis_id.empty() == false )
        {
            string query = R"-(//head/*[@data-mesis_id="{mesis_id}"])-";
            replaceAll( query, "{mesis_id}", mesis_id );
            val doE = head.x_( query );

            if( doE != js_null())
            {   d_get( s( "already E: elem w/ mesis_id " ) + mesis_id, s( "skipped" ));
                continue;
            }
        }

        if( s( elem_[ "tagName" ]) != s( "SCRIPT" ))
            head.appendChild( elem_.clone());
        else
        {   val script_copy = __copy_script( elem_.inst );
            head.appendChild( script_copy );
        }

        if( cache_el != js_null())
            cache_el.call< void >( "append", elem_.clone().inst );
    }
}


void cache_over_async( string url, string selector, string text )
{
if( stack_trace ) console[ "log" ]( s( "Mesis: " ) + s( __func__ ));

    if( can_run.find( "cache_overwrite" ) == can_run.end()
        || can_run[ "cache_overwrite" ].find( "|cache_over_async|" ) == string::npos )
    {   std::cout << __func__ << " " << err_dont_call_directly << std::endl;
        return;
    }
    can_run_exclude( "cache_overwrite", "cache_over_async" );

    DomInterface tgt = { html.call< val >( "elem", selector )};

    string viewmodel = s( "<viewmodel>" ) + tgt.get( "innerHTML" ) + s( "</viewmodel>" );

    val headers = jo({{ "Content-Type", "text/html" }});
    val options = jo({{ "status", "200" }, { "statusText", "OK" }});
    options.set( "status", 200 );
    options.set( "headers", headers );

    string html_ = "";
    if( text.find( "</upstream>" ) != string::npos )
        html_ = text.substr( 0, text.find( "</upstream>" ) + 11 );
    html_ += viewmodel;

    if( text.find( "<downstream>" ) != string::npos )
        html_ += text.substr( text.find( "<downstream>" ));

    val syntResp = g_[ "Response" ].new_( html_, options );

    cacheAPI.call< val >( "put", url, syntResp );
}


void cache_overwrite( string selector )
{
if( stack_trace ) console[ "log" ]( s( "Mesis: " ) + s( __func__ ));

    DomInterface tgt = { elem( selector )};
    bool error_ = false;
    string err = "Mesis: cache overwrite called, selector {selector} results in null";

    string url = "";
    if( tgt.inst == js_null())
        error_ = true;
    else
    {   url = tgt.get( "data-url" );
        if( url.size() == 0 )
        {   error_ = true;
            err = "Mesis: cache overwrite called, selector {selector} without data-url";
        }
    }

    if( error_ )
    {   replaceAll( err, "{selector}", selector );
        console[ "error" ]( err );

        if( stack_trace ) console[ "log" ]( s( "Mesis: exit(error) " ) + s( __func__ ));

        return;
    }

    g_.set( "ffacet_cache", cacheAPI );
    can_run[ "cache_overwrite" ] = "|cache_over_async|";
    EM_ASM(
    {   const selector = UTF8ToString( $0 ).trim();
        const url = g_.elem( selector ).dataset.url;

        globalThis[ "ffacet_cache" ].match( url )
            .then( response =>
            {   if( response )
                    return response.text();
                else
                    return "";
            })
            .then( text =>
            {   MesisAgent.cache_over_async( url, selector, text );
            });
            // TODO: catch aqui? qdo a cache API levanta excecao? por ex se
            // chamarmos o overwrite para uma entrada q nao existe, ela retorna ""
    },
    selector.c_str());

if( stack_trace ) console[ "log" ]( s( "Mesis: exit " ) + s( __func__ ));
}


void cache_remove( string selector )
{
if( stack_trace ) console[ "log" ]( s( "Mesis: " ) + s( __func__ ));

    DomInterface tgt = { elem( selector )};
    bool error_ = false;
    string err = "Mesis: cache remove called, selector {selector} results in null";

    string url = "";
    if( tgt.inst == js_null())
        error_ = true;
    else
    {   url = tgt.get( "data-url" );
        if( url.size() == 0 )
        {   error_ = true;
            err = "Mesis: cache remove called, selector {selector} without data-url";
        }
    }

    if( error_ )
    {   replaceAll( err, "{selector}", selector );
        console[ "error" ]( err );

        if( stack_trace ) console[ "log" ]( s( "Mesis: exit(error) " ) + s( __func__ ));

        return;
    }

    g_.set( "ffacet_cache", cacheAPI );
    can_run[ "cache_overwrite" ] = "|cache_over_async|";
    EM_ASM(
    {   const selector = UTF8ToString( $0 ).trim();
        const url = g_.elem( selector ).dataset.url;

        globalThis[ "ffacet_cache" ].delete( url );
    },
    selector.c_str());

if( stack_trace ) console[ "log" ]( s( "Mesis: exit " ) + s( __func__ ));
}



void cache_clone_async( string new_url, string text )
{
    // run cap
    if( can_run.find( "cache_clone" ) == can_run.end())
    {   std::cout << __func__ << " " << err_dont_call_directly << std::endl;
        return;
    }
    can_run.erase( "cache_clone" );

    val headers = jo({{ "Content-Type", "text/html" }});
    val options = jo({{ "status", "200" }, { "statusText", "OK" }});
    options.set( "status", 200 );
    options.set( "headers", headers );

    val syntResp = g_[ "Response" ].new_( text, options );

    cacheAPI.call< val >( "put", new_url, syntResp );
}


void cache_clone( string url, string new_url )
{
    g_.set( "ffacet_cache", cacheAPI );
    can_run[ "cache_clone" ] = "|cache_clone|";

    EM_ASM(
    {   const url = UTF8ToString( $0 ).trim();
        const new_url = UTF8ToString( $1 ).trim();

        globalThis[ "ffacet_cache" ].match( url )
            .then( response =>
            {   if( response )
                    return response.text();
                else
                    return "";
            })
            .then( text =>
            {   MesisAgent.cache_clone_async( new_url, text );
            });
            // TODO: catch aqui? qdo a cache API levanta excecao? por ex se
            // chamarmos o overwrite para uma entrada q nao existe, ela retorna ""
    },
    url.c_str(), new_url.c_str());
}


void insert_island( string addr )
{
    if( islands_store.find( addr ) == islands_store.end())
    {
        std::cerr << "insert_island: addr not found on queue" << std::endl;
        return;
    }

    string text = s( islands_store[ d_get( addr, "endereco da ilha" )]);
    d_get( text.substr( 0, 100 ), "codigo da ilha a ser inserida" );

    // now that we have the text, remove the entry from the island_store
    islands_store.erase( addr );

    auto params = split( addr, "::" );
    string url = params[ 0 ];
    string selector = params[ 1 ];

    val parser = g_[ "DOMParser" ].new_();

    DomInterface island =
    {   parser.call< val >
        ( "parseFromString", text, string( "text/html" ))
    };

    // if the selector contains "ext|", it is an addr in the kv_store,
    // used to allow Mesis islands to be injected in detached doc frags
    val tgt = js_null();
    if( selector.find( "ext|" ) != string::npos )
    {
        string ext_node = selector;
        replaceAll( ext_node, "ext|", "" );
        tgt = kv_get( ext_node );
    }
    else
        tgt = elem( selector ); // or the tgt is just a part of the view model

    if( island[ "documentElement" ] == js_null())
    {
        std::cerr << "Mesis insert_island: malformed or absent (no documentElement resulted) "
                << url << std::endl;

        string error_html = s( "Empty Island</h1><br/>" ) + url
                             + s( "<br/><br/><span>Malformed or absent</span></div>" );
        tgt[ "dataset" ].set( "url", s( "#ERROR#" ));
        tgt.set( "innerHTML", error_html );
        return;
    }

    island.inst = island[ "documentElement" ];

    // CSS (style and link tags) can be injected before or after the HTML generation,
    // dependending on their relative position from Mesis main script in the source file,
    // to mimic the normal parsing rules of the browsers
    val elements_ = island.x_( "[]://style | //link[@rel='stylesheet'] | //link[@rel='preconnect'] | //script" );

    // if the document comes from the cache, we have a specific format, divided in
    // <upstream>, <viewmodel>, <downstream>
    bool generate_html = true;
    auto& no_cache = islands_no_cache;
    if( no_cache.find( url ) == no_cache.end() || no_cache[ url ] == false )
    {   string island_downloaded_entry = s( "island_downloaded::" ) + url;
        // if it was downloaded before (the kv_store entry is a flag, page is in Cache API)
        if( kv_get( island_downloaded_entry ) != js_null())
        {   generate_html = false;
            elements_ = island.x_( "[]://upstream/*" );
        }
    }

    DomInterface head = { doc[ "head" ]};

    // check if we had in the doc's head elements that must be removed
    // temp elements must have a data-dependency="url" attribute. Any element
    // that has one that DO NOT list the current url is removed
    curate_head( url, js_null(), js_null());

    // if we are generating html, we have to create the cache entry with upstream/downstream,
    // if we are getting txt from the cache, they already E
    DomInterface upstream = {( generate_html
                                ? parser.call< val >( "parseFromString", s( "<upstream></upstream>" ),
                                                        s( "text/html" ))[ "documentElement" ]
                                : js_null())};

    // first insert elements that appears above the Mesis script on the sorce file. Different from
    // the mains flow, an island may have tag that must be injected before the html generation
    int lim = elements_[ "length" ].as< uint32_t >();

    for( int32_t c = 0; c < lim; c++ )
    {
        DomInterface node = { elements_[ c ]};

        val regexp = g_[ "RegExp" ].new_( s( R"-([\s]*Mesis[\s]*\[[\s]*_[\s]*`html)-" ));
        string code = s( node[ "innerHTML" ]);

        // stop if we found the main script - in this block we add only the elements found
        // before the main script in the island file
            // notice that if the document is coming from the cacheAPI, there will be no Mesis
        // script, so all the tags will be processed and only the delayed ones will be skipped,
        // but all that was previously computed and stored in the <upstream> custom tag
        if( regexp.call< bool >( "test", code ) == true )   break;

        // the ones with a data-delayed attribute are postponed even if appear before the
        // main script
        string is_delayed = node.get( "data-delayed" );
        if( is_delayed.empty() == false ) continue;

        curate_head( url, node.inst, upstream.inst != js_null()
                                        ? upstream.el_( "upstream" ) : js_null());
    }

    // insert Mesis script (that will generate the island HTML)
        // notice that if the document is coming from the cache API, this block will not run
    for( int32_t c = 0; c < lim && generate_html; c++ )
    {
        DomInterface node = { elements_[ c ]};

        val regexp = g_[ "RegExp" ].new_( s( R"-([\s]*Mesis[\s]*\[[\s]*_[\s]*`html)-" ));
        string code = s( node[ "innerHTML" ]);

        if( regexp.call< bool >( "test", code ) == false )   continue;

        // ja se verificou se o objeto Mesis[_`html`] esta definido, entao injeta
        val main_script_copy = __copy_script( node.inst );
        head.appendChild( main_script_copy );

        // converte Mesis (JS) para HTML
        ref_terms_list = _get_terms_list();  // tb reconstroi o mapa geral no lado do c++ antes de continuar

        val html_v = _mesis_rebuild_html_hierarchy();

        string new_html = "";

        if( html_v == js_null())
        {
            std::cerr << "Mesis insert_island: no HTML generated. " << url << std::endl;

            string error_html = s( "Empty Island</h1><br/>" )
                            + url + s( "<br/><br/><span>No HTML generated</span></div>" );

            tgt[ "dataset" ].set( "url", s( "#ERROR#" ));
            tgt.set( "innerHTML", error_html );
            return;
        }
        else
            new_html = mesis_to_html( "html", html_v );

        // apaga o objeto Mesis, exceto os objetos marcados com a decoracao "keep"
        // (e o html, q eh sempre removido)
        _erase_mesis_object();

        // injeta o conteudo no seletor indicado
        tgt.set( "innerHTML", new_html );
        tgt[ "dataset" ].set( "url", url );

        // remove os scripts usados para gerar o HTML
        node.inst.call< void >( "remove" );
        main_script_copy.call< void >( "remove" );
        break;
    }

    // if the document is coming from the cache, we have generated the html before and stored
    // in in a custom tag "viewmodel"
    if( !generate_html )
    {   tgt.set( "innerHTML", s( island.el_( "viewmodel" )[ "innerHTML" ]));
        tgt[ "dataset" ].set( "url", url );
    }

    // insert now all elements that appears after the Mesis script in the source file,
    // and the delayed ones, this ones regardless of their position in the file.
        // if the document comes from the cache, we have a specific format, divided in
    // <upstream>, <viewmodel>, <downstream>
        // if it was not requested as no-cache
    if( no_cache.find( url ) == no_cache.end() || no_cache[ url ] == false )
    {   string island_downloaded_entry = s( "island_downloaded::" ) + url;
            // if it was downloaded before
        if( kv_store.find( island_downloaded_entry ) != kv_store.end())
            elements_ = island.x_( "[]://downstream/*" );
    }

    DomInterface downstream =
    {( generate_html
        ? parser.call< val >( "parseFromString", s( "<downstream></downstream>" ),
                                string( "text/html" ))[ "documentElement" ]
        : js_null())
    };
    bool already_inserted = true;
    lim = elements_[ "length" ].as< int >();
    for( int32_t c = 0; c < lim; c++ )
    {
        DomInterface node = { elements_[ c ]};
        d_get( s( node[ "tagName" ]), "apos, testando elemento" );

        val regexp = g_[ "RegExp" ].new_( s( R"-([\s]*Mesis[\s]*\[[\s]*_[\s]*`html)-" ));
        string code = s( node[ "innerHTML" ]);

        // se achamos o  principal, começa a inserir os subsequentes
        if( generate_html == false
                || ( already_inserted && regexp.call< bool >( "test", code ) == true ))
        {   already_inserted = false;
            d_get( "", "apos, encontrado Mesis main, insere elems apos" );
            continue;
        }

        string is_delayed = d_get( node.get( "data-delayed" ), "apos, elemento eh delayed?" );

        // pula os ja inseridos, executa somente delayed ou posteriores no arquivo
        if( already_inserted == true && is_delayed.empty())   continue;

        d_get( s( node[ "outerHTML" ]).substr( 0, 100 ), "apos, inserindo elemento" );

        curate_head( url, node.inst, downstream.inst != js_null()
                                        ? downstream.el_( "downstream" ) : js_null());
    }

    // create the cache entry if needed
    if( generate_html )
    {       // if it was not requested as no-cache
        if( no_cache.find( url ) == no_cache.end() || no_cache[ url ] == false )
        {   string island_downloaded_entry = s( "island_downloaded::" ) + url;
                // if it was NOT downloaded before
            if( kv_store.find( island_downloaded_entry ) == kv_store.end())
            {
                val cache_map = g_[ "Map" ].new_();
                cache_map.call< void >( "set", island_downloaded_entry, true );
                kv_add( cache_map );

                val headers = jo({{ "Content-Type", "text/html" }});
                val options = jo({{ "status", "200" }, { "statusText", "OK" }});
                options.set( "status", 200 );
                options.set( "headers", headers );

                string html_ = upstream.inst != js_null()
                                ? s( upstream.el_( "upstream" )[ "outerHTML" ]) : "";
                html_ += s( "<viewmodel>" ) + s( tgt[ "innerHTML" ]) + s( "</viewmodel>" );
                html_ += downstream.inst != js_null()
                            ? s( downstream.el_( "downstream" )[ "outerHTML" ]) : "";

                val syntResp = g_[ "Response" ].new_( html_, options );

                cacheAPI.call< val >( "put", url, syntResp );
            }
        }
    }

    // remove from cache if we have a invalidation (no_cache map entry set to true)
        // if it was declared no-cache but afterwards invalidated
    if( islands_no_cache.find( url ) != islands_no_cache.end()
            || islands_no_cache[ url ] == true )
    {   string island_downloaded_entry = s( "island_downloaded::" ) + url;
            // if it was downloaded before
        if( kv_store.find( island_downloaded_entry ) != kv_store.end())
        {   // remove from the cache
            cacheAPI.call< val >( "delete", url );
            kv_remove( island_downloaded_entry );
        }
    }

    // custom event to inform that the island was injected
    val detail = jo({{ "url", url }, { "selector", selector },
                     { "ts", std::to_string( local_time )},
                     { "page", "dummy" }});

    // if we have a detached doc frag, dont copy the page in the event detail because
    // we can have a lot of data - the detached frag idea is to serve as a database.
    if( selector.find( "ext|" ) == string::npos )
        detail.set( "page", clone( tgt ));
    else
        detail.set( "page", js_null());

    val options = jo({{ "detail", "dummy" }});
    options.set( "detail", detail );
    val eventoCarga = g_[ "CustomEvent" ].new_( s( "islandLoaded" ), options );
console[ "log" ]( s( "dispatching event for " )+ url );
    window.call< void >( "dispatchEvent", eventoCarga );
}


bool Mesis_internal_loop( double time, void* userData )
{
    local_time = static_cast< uint32_t >( global_v[ "Date" ].call< double >( "now" ) / 1000. );
    frame_counter++;

    // check if we have to run the loading procedure for Mesis islands
    for( int run_once = 0;
        island_addrs.size() > 0 && run_once < 1; run_once++ )
    {
        // flag para segurar a injecao da ilha ate estar tudo pronto.
        // (para esperar carga de modulos JS por ex)
        if( kv_store.find( "insert_island_hold" ) != kv_store.end())
            break;

        int len = island_addrs.size();

        // loop through all the addresses to find the next to download
        for( int c = 0; c < len; c++ )
        {
            string island_params = island_addrs[ c ];
            auto params = split( island_params, "::" );

            // param is url::selector, but if it was already issued and downloading,
            // it will be changed to fetch::url::selector
            if( params.size() == 2 )
            {
                string selector = params[ 1 ];

                // if we have ext| in selector, it is an address on the kv_store,
                // so islands can also be injected on dettached doc frags
                val tgt = js_null();
                if( selector.find( "ext|" ) != string::npos )
                {
                    string ext_node = selector;
                    replaceAll( ext_node, "ext|", "" );
                    tgt = kv_get( ext_node );
                }
                else
                    tgt = elem( selector );
                d_get( selector, "island para fetch neste seletor" );

                if( tgt == js_null())
                {
                    std::cerr << "fetch_island: target selector result in null: " << selector
                                << " -> url: " << params[ 0 ] << std::endl;
                    island_addrs.erase( island_addrs.begin() + c );
                    continue;
                }

                // safe to del inside for loop because we process a single entry per Mesis loop
                island_addrs.erase( island_addrs.begin() + c );
                island_addrs.push_back( s( "fetch::" ) + island_params );

                // don't download if it is in the cache
                    // if it was not requested as no-cache
                auto& no_cache = islands_no_cache;
                if( no_cache.find( params[ 0 ]) == no_cache.end() || no_cache[ params[ 0 ]] == false )
                {   string island_downloaded_entry = s( "island_downloaded::" ) + params[ 0 ];
                        // if it was downloaded before
                    if( kv_store.find( island_downloaded_entry ) != kv_store.end())
                    {   val resp = jo({{ "text", "" }}); // empty response and the fn will search the cache
                        island_response( params[ 0 ], selector, resp, false ); /* error = false */
                        break;
                    }
                }

                EM_ASM(
                {   var url = UTF8ToString( $0 ).trim();
                    var selector = UTF8ToString( $1 ).trim();

                    fetch( url ).then( response => response.text())
                    .then( text =>
                    {   const no_error = false;
                        MesisAgent.island_response( url, selector, { text: text }, no_error );
                    })
                    .catch( err =>
                    {   const error = true;
                        MesisAgent.island_response( url, selector, err, error );
                    });
                }, params[ 0 ].c_str(), params[ 1 ].c_str());

                break;
            }
        }
    }

    // check if we have to run the injection procedure for Mesis islands
    for( int run_once = 0;
        island_addrs.size() > 0 & run_once < 1; run_once++ )
    {
        // flag to hold the process (to wait the load of a JS module for instance)
        if( kv_store.find( "insert_island_hold" ) != kv_store.end())
            break;

        int len = island_addrs.size();
        for( int c = 0; c < len; c++ )
        {
            string island_params = island_addrs[ c ];
            auto params = split( island_params, "::" );

            // param is url::selector, but if it was already issued and downloading,
            // it will be changed to fetch::url::selector
            if( params.size() > 2 )
            {
                // check if download finished
                string addr = params[ 1 ] + s( "::" ) + params[ 2 ];

                // address appears in island_store only when download finishes or we
                // retrieve the content from the cache API
                if( islands_store.find( addr ) != islands_store.end())
                {
                    island_addrs.erase( std::remove( island_addrs.begin(), island_addrs.end(),
                                        island_params ), island_addrs.end());
                    insert_island( addr );
                    break;
                }
            }
        }
    }

    // se existem funcoes registradas para rodar, verifica se tem q executar
    if( !loop_stoped )
    for( auto& code_decl : on_animation_frame )
    {
        string function_name = code_decl.first;

        // check if the auto-removal parameter E
        string loop_cap_key = function_name + s( "::meta::if" );
        bool run_once = true;
        DomInterface tgt = { js_null()};
        for( ; run_once; run_once = false )
        {
            // E, check if the condition for running still holds
            val loop_cap_v = kv_store[ loop_cap_key ];
            if( loop_cap_v == js_null())   break;
            string loop_cap = s( loop_cap_v );

            auto parts = split( loop_cap, "|" );
            tgt.inst = elem( parts[ 0 ]);

            string err_msg = "condition cannot be tested, function will continue to run.";
            if( tgt.inst == js_null())
            {   string err_ = s( "Run condition exists for fn " ) + function_name + s( " however " );
                err_ += s( "target selector " ) + parts[ 0 ] + s( " results in null: " );
                console[ "log" ]( err_ + err_msg );
                break;
            }
            try 
            {   loop_cap = s( tgt.inst );
            } 
            catch(...)
            {   string err_ = s( "Run condition exists for fn " ) + function_name + s( " however " );
                err_ += s( "target selector " ) + parts[ 0 ] + s( " dont results in string: " );
                console[ "log" ]( err_ + err_msg );
                break;
            }
            if( loop_cap.find( parts[ 1 ]) == string::npos )
            {   Mesis_remove_from_loop( function_name );
                break;
            }
        }
        if( tgt.inst != js_null() && run_once )   continue;

        std::pair< int16_t, int16_t > exec_cycle = code_decl.second;
        int16_t frame_window = exec_cycle.first;
        int16_t frame_pos = exec_cycle.second;

        // frame_window > 0 indica execucao periodica
        // frame_window < 0 indica contagem de frames
        bool run_code = false;
        if( frame_window > 0 )
        {   if( frame_counter % frame_window == frame_pos )
                run_code = true;
        }
        else
        {   if( frame_pos == 0 )
            {   frame_pos = ( frame_window * -1 ) - 1;
                run_code = true;
            }
            else
            {   frame_pos--;
                on_animation_frame[ function_name ] = { frame_window, frame_pos };
            }
        }

        if( run_code )
        {   val code = g_[ function_name ];
            if( code == js_null())
            {
                // se nao eh uma global, testa pra ver se temos armazenada a funcao na kv_store
                if( kv_store.find( function_name ) != kv_store.end())
                    code = kv_store[ function_name ];
                else
                    on_animation_frame[ function_name ] = { 0, -1 };
            }
            if( code != js_null())
                g_.call< int >( "requestAnimationFrame", code );
        }
    }

    emscripten_request_animation_frame_loop( Mesis_internal_loop, nullptr );
    return false;
}


// funcao principal q traduz a pagina inicial na notacao Mesis para html. main() do modulo
// roda antes e prepara os objetos para esta, o html_src_v (q é o objeto global Mesis) e
// o script identificado como principal é marcado com data-mesis_main
void start()
{
    // itera pelo mapa de componentes de seletor q foi construido. Como a start deve
    // ser chamada somente na 1a carga, o mapa deve conter somente a lista de nome de propriedades
    // calculadas pela funcao _ (funcao underscore) no Objeto Mesis
        // Para cada item, verifica se eh uma folha ou tem ele mesmo referencias a outros itens
    // (folhas na arvore q representa o HTML a ser gerado sao objetos sem referencias a outros)
        // ref_terms_list eh inicializado aqui
    ref_terms_list = _get_terms_list();   // tb reconstroi o mapa geral no lado do c++ antes de continuar

    // substitui as referencias para criar a estrutura hierarquica desajeitada
    // do HTML, mas ainda em js
    val html_v = _mesis_rebuild_html_hierarchy();

    if( html_v == js_null())
    {
        char btick = static_cast< char >( 96 );

        // o html nao foi gerado, TODO: a unica possibilidade eh nao ter criado o obj html?
        std::cerr << "In the main Mesis block the first declaration (first use of the _ function ) "
                    << "must be Mesis[_" << btick << "html" << btick << "] = { ..." << std::endl;
        return;
    }

    string new_html = mesis_to_html( "html", html_v );

    // apaga o objeto Mesis (src do html), exceto os objetos marcados com a decoracao "keep"
    // (a prop html, q eh sempre removida - nao pode receber um keep)
    for( auto it = ref_term_tokens.begin(); it != ref_term_tokens.end(); )
    {
        string term = it->first;

        string is_html_tag = "";
        if( term.find( "__" ) != string::npos )
            is_html_tag = term.substr( term.find( "__" ));

        if( html_src_v[ "decoration" ][ term ] == js_null() || is_html_tag == "__html" )
        {
            it = ref_term_tokens.erase( it );
            html_src_v.call< void >( "delete", term );
            global_v[ "__mesis_ref_term_tokens" ].call< void >( "delete", term );

            auto it2 = std::remove( ref_terms_list.begin(), ref_terms_list.end(), term );
            ref_terms_list.erase( it2, ref_terms_list.end());
        }
        else ++it;
    }

    // separa head e body, se existe um head
    string new_head = "";
    string head_close = "";

    val parser = g_[ "DOMParser" ].new_() ;

    val fullpage = parser.call< val >
    ( "parseFromString", new_html, string( "text/html" ));

    if( fullpage[ "head" ] != js_null())
    {
        val head_elems = fullpage[ "head" ][ "children" ];
        uint32_t len_ = head_elems[ "length" ].as< uint32_t >();

        for( int32_t c = 0; c < len_; c++ )
        {   val elem_ = fullpage[ "head" ][ "children" ][ c ];
            doc[ "head" ].call< void >( "appendChild", elem_.call< val >( "cloneNode", true ));
        }
    }

    doc[ "body" ].call< void >( "replaceWith", fullpage[ "body" ]);

    // remove o script q foi usado para gerar o HTML
    val mesis_main = doc[ "head" ].call< val >( "elem", s("[data-mesis_main]" ));
    if( mesis_main != js_null())
        mesis_main.call< void >( "remove" );

    mesis_main = doc[ "head" ].call< val >( "elem", s( "#mesis_main" ));
    if( mesis_main != js_null())
        mesis_main.call< void >( "remove" );

    // por ultimo, ativa os scripts que devem rodar apos a geracao do HTML (dentro
    // de templates, marcados com delayed, q nao sejam este q ja executou)
    val templates_ = g_.call< val >( "elem", s( "[]:template" ));
    uint32_t lim = templates_[ "length" ].as< uint32_t >();

    val delayed = g_[ "Array" ].new_();
    for( uint32_t c = 0; c < lim; c++ )
    {
        val scripts = templates_[ c ][ "content" ][ "children" ];
        int len = scripts[ "length" ].as< uint32_t >();

        for( uint32_t i = 0; i < len; i++ )
        {
            DomInterface script = { scripts[ i ]};
            string is_delayed = script.get( "data-delayed" );
            d_get( s( "|" ) + is_delayed + s( "|" ), "is delayed? " );

            string is_mesis = script.get( "data-mesis_main" );

            d_get( s( script[ "outerHTML" ]).substr( 0, 100 ), "delayed script content" );
            if( d_get( s( script[ "tagName" ]), "elem no template" ) == s( "SCRIPT" )
                    && is_delayed.empty() == false
                    && d_get( is_mesis, "is Mesis main script?" ).empty())
                delayed.call< void >( "push", scripts[ i ]);
        }
    }

    lim = delayed[ "length" ].as< uint32_t >();

    for( int32_t c = 0; c < lim; c++ )
    {
        d_get( "", "encontrado script delayed no fluxo principal" );
        DomInterface script = { delayed[ c ]};

        // aqui nao checa o data-mesis_id, pq nao faz sentido proteger contra inserir
        // 2x o mesmo script no fluxo principal do Mesis (ter 2 copias no mesmo arquivo)
        val new_script = __copy_script( script.inst );
        doc[ "head" ].call< void >( "appendChild", new_script );

        // cleanup: remove os script de dentro do template...
        // TODO: DEBUG: marcar para remover, verificar se a array delayed
        // nao acaba invalidada se removemos os elementos aqui
        script.inst.call< void >( "remove" );
    }

    // ... e remove todos os templates q ficarem vazios
    templates_ = g_.call< val >( "elem", s( "[]:template" ));
    lim = templates_[ "length" ].as< uint32_t >();

    for( int32_t c = 0; c < lim; c++ )
    {
        DomInterface template_ = { templates_[ c ]};

        val content = template_[ "content" ];
        val elems = content[ "children" ];
        uint32_t lim2 = elems[ "length" ].as< uint32_t >();

        if( lim2 == 0 )
            template_.inst.call< void >( "remove" );
    }
}


bool loop_is_running = false;


bool allow_cache_API = false;

void set_cacheAPI( val cache )
{
    if( !allow_cache_API )
    {   std::cout << __func__ << " " << err_dont_call_directly << std::endl;
        return;
    }
    allow_cache_API = false;

    cacheAPI = cache;
}


int main()
{
    if( loop_is_running )
    {
        std::cerr << "Mesis already initialized" << std::endl;
        return 0;
    }
    loop_is_running = true;

    allow_cache_API = true;
    val cache_prom = g_[ "caches" ].call< val >( "open", s( "fable-facet-islands" ))
        .call< val >( "then", self( set_cacheAPI ));

    // injeta a funcao _ e as auxiliares e aliases
    EM_ASM(
    {
        // o delete serve para evitar um ES_ASM({ delete... })
        Mesis  = { decoration: {}};
        Mesis[ "delete" ] = function( term )
        {   // colocar diretamente acima esta funcao causa erro de compilacao (?)
            delete Mesis[ term ];
        };

        // o mapa de seletores mantem na chave um seletor completo, e como
        // valor uma string com os seletores semi-interpretados na forma
        // =tagname<>.classes<>#id para uso no wasm
            // __mesis_ref_term_tokens eh inicializada aqui
        __mesis_ref_term_tokens = { "delete": function( term ){ delete __mesis_ref_term_tokens[ term ]}};

        // para garantir a ordem de insercao e q cada item seja unico
        __mesis_idx = 0;

        // @note pega um seletor no formato tagName.class1[...].classn#id e
        // retorna o formato =tagName<>.class1[...]classn<>#id, e adiciona a tag
        // em __mesis_ref_term_tokens, lista de tags a serem geradas
        mesis_parse_tag = function( selector )
        {
            // se a tag ja tem o prefixo _nnnnn__, separa
            var prefix = "";
            const pref_rx = new RegExp( "_[0-9]{5}__", "g" );
            let pref = selector.matchAll( pref_rx ).toArray();
            if( pref.length > 0 )
            {
                prefix = pref[ 0 ][ 0 ];
                selector = selector.substring( prefix.length );
            }

            const regex = new RegExp( "[.#]", "g" );

            let parts = "";
            let ms = selector.matchAll( regex ).toArray();
            if( ms.length > 0 )
            {
                if( ms[ 0 ].index > 0 )
                    parts += "=" + selector.substring( 0, ms[ 0 ].index ) + "<>";

                for( let i = 0; i < ms.length; i++ )
                {   parts += ms[ i ][ 0 ];

                    if( i < ( ms.length - 1 ))
                    {
                        // o formato sera =tag<>.classes<>#id
                        parts += selector.substring( ms[ i ].index + 1, ms[ i + 1 ].index ).trim();
                        parts += "<>";
                    }
                    else
                        parts += selector.substring( ms[ i ].index + 1 ).trim();
                }
            }
            else parts = "=" + selector;

            if( prefix == "" )
                selector = "_" + ( __mesis_idx.toString()).padStart( 5, "0" ) + "__" + selector;
            else
                selector = prefix + selector;

            __mesis_idx++;
            __mesis_ref_term_tokens[ selector ] = parts;

            return selector;
        };


        elem = function( selector )
        {
            var res = null;
            if( typeof selector == "object" )
            {
                if( "target" in selector )
                {   if( selector?.target )
                       res = selector.target;
                }
                return res;
            }
            selector = selector.toString().trim();
            if( selector.substr( 0, 3 ) == "[]:" )
            {
                try
                {   res = document.querySelectorAll( selector.substr( 3 ));
                }
                catch( e ){};
            }
            else
            {   try
                {   res = document.querySelector( selector );
                }
                catch( e ){};
            }
            return res;
        };


        e_ = function( strings, ...values )
        {
            if( strings == undefined || strings.length == 0 )
                return null;

            if( strings.lenght == 1 && strings[ 0 ] == "html" )
                return document.documentElement;

            if( strings.lenght == 1 && strings[ 0 ] == "" )
                return document.body;

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            res = res.trim();
            if( res.substr( 0, 3 ) == "[]:" )
                return document.querySelectorAll( res.substr( 3 ));
            else
                return document.querySelector( res );
        };


        new_elem = function( tagname )
        {
            if( tagname == undefined
                    || ( tagname.indexOf( "." ) == -1 && tagname.indexOf( "#" ) == -1 ))
            {   var elem_name = ( tagname == undefined ? "div" : tagname );
                return document.createElement( elem_name );
            }

            // pega um seletor no formato tagName.class1[...].classn#id e
            // retorna o formato =tagName<>.class1[...]classn<>#id, e adiciona a tag
            // em __mesis_ref_term_tokens, lista de tags a serem geradas
            var mesis_sel = mesis_parse_tag( tagname );
            var parsed = __mesis_ref_term_tokens[ mesis_sel ];

            // como so queriamos o parse mas nao inserir outra tag para ser gerada, apaga
            delete __mesis_ref_term_tokens[ mesis_sel ];
            parsed = parsed.split( "<>" );

            var tag = parsed.filter( el => el.includes( "=" ));
            var elem_ = null;
            if( tag.length > 0 )
                elem_ = document.createElement( tag[ 0 ].replace( "=", "" ).trim());
            else
                elem_ = document.createElement( "div" );

            var id = parsed.filter( el => el.includes( "#" ));
            if( id.length > 0 )
                elem_.id = id[ 0 ].replace( "#", "" ).trim();

            var classes = parsed.filter( el => el.includes( "." ));
            classes.forEach( class_ =>
            {   elem_.classList.add( class_.replace( ".", "" ).trim()) });

            return elem_;
        };

        // TODO: incluir a sintaxe [n]: para retornar um document fragment com n
        // dos elementos informados
        new_e_ = function( strings, ...values )
        {
            if( strings == undefined || strings.length == 0 )
                return new_elem();

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            return new_elem( res );
        };


        g_ = globalThis;


        Element.prototype.elem = function( selector )
        {
            selector = selector.trim();
            var res = null;

            try
            {   if( selector.substr( 0, 3 ) == "[]:" )
                    res = this.querySelectorAll( selector.substr( 3 ));
                else
                    res = this.querySelector( selector );
            }
            catch(e)
            {}

            return res;
        };


        Element.prototype.e_ = function( strings, ...values )
        {
            if( strings == undefined || strings.length == 0 )
                return null;

            if( strings.lenght == 1 && "|self|this|".includes( strings[ 0 ]))
                return this;

            if( strings.lenght == 1 && ( strings[ 0 ] == "." || strings[ 0 ] == "" ))
                return this;

            if( strings.lenght == 1 && strings[ 0 ] == "" )
                return this;

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            res = res.trim();
            if( res.substr( 0, 3 ) == "[]:" )
                return this.querySelectorAll( res.substr( 3 ));
            else
                return this.querySelector( res );
        };


        xpath_ = function( query, context )
        {
            if( context == null )
                context = document;

            var return_array = false;
            if( query.substr( 0, 3 ) == "[]:" )
            {
                return_array = true;
                query = query.substr( 3 ).trim();
            }

            var xobj = null;
            // 7 = XPathResult.ORDERED_NODE_SNAPSHOT_TYPE: snapshot de nos na ordem q foram achados
            try
            {   xobj = document.evaluate( query, context, null, 7, null ); }
            catch( e ){};

            if( xobj )
            {
                var ret_val = new Array();
                for( var i = 0; i< xobj.snapshotLength; i++ )
                    ret_val.push( xobj.snapshotItem( i ));

                if( return_array )
                    return ret_val;

                if( ret_val.length == 0 )
                    return null;

                if( ret_val.length == 1 )
                    return ret_val[ 0 ];

                return ret_val;
            }

            // se chegou aqui eh pq nao achou noh algum, entao nesse caso
            // a query pode estar atras de um dado bruto
            try
            {   xobj = document.evaluate( query, context, null, 0, null );   }
            catch( e ){};

            if( xobj )
            {   // nesse caso nao retorna array
                switch( xobj.resultType )
                {
                    case 1:{  return xobj.numberValue;  }
                    case 2:{  return xobj.stringValue;  }
                    case 3:{  return xobj.booleanValue; }
                    default: {  return xobj.toString()  }
                }
            }
            else return null;
        };

        Element.prototype.xpath = function( query )
        {
            var res = null;

            try
            {   res = xpath_( query, this );
            }
            catch(e)
            {}

            return res;
        };


        Element.prototype.x_ = function( strings, ...values )
        {
            if( strings == undefined || strings.length == 0 )
                return null;

            if( strings.lenght == 1 && strings[ 0 ] == "" )
                return null;

            if( strings.lenght == 1 && strings[ 0 ] == "." )
                return this;

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            return this.xpath( res );
        };


        HTMLElement.prototype.css_var = function( var_name, value = undefined )
        {
            const elem = this;
            var vname = var_name;

            if( var_name.substr( 0, 2 ) != "--" ) vname = "--" + vname;

            if( value != undefined )
                elem.style.setProperty( vname, value );
            else
            {   const compCss = getComputedStyle( elem );
                return compCss.getPropertyValue( vname );
            }
        };


        HTMLElement.prototype.css_var_get = function( var_name )
        {
            const elem = this;
            var vname = var_name;

            if( var_name.substr( 0, 2 ) != "--" ) vname = "--" + vname;

            const compCss = getComputedStyle( elem );
            return compCss.getPropertyValue( vname );
        };


        HTMLElement.prototype.get_css_prop = function( prop_name )
        {
            const elem = this;
            const compCss = getComputedStyle( elem );
            return compCss.getPropertyValue( prop_name );
        };


        Element.prototype.empty = function()
        {   return this.children.length == 0;  };


        Element.prototype.add = function( element )
        {   this.append( element );  };


        // se duration > 0, aplica, e apos o intervalo, remove o estilo (faz undo)
        // se duration = 0, aplica
        // se duration < 0, aplica apos -duration
        mesis_cascade = async function( css_class_or_style, duration = 0 )
        {
            if( duration < 0 )
                await new Promise( resolve => setTimeout( resolve, -duration ));

            const css_ = css_class_or_style.trim();
            const revsol = String.fromCharCode( 92 );
            var regex = RegExp( revsol + "bdel ", "i" );

            // styles
            if( regex.test( css_ ) || css_.includes( ":" ))
            {
                var styles = css_class_or_style.split( ";" );

                var style_prop = "";
                if( this.hasAttribute( "style" ))
                    style_prop = this.attributes[ "style" ].value;

                var old_style = style_prop;

                styles.forEach( style_ =>
                {
                    var style = style_.trim();
                    if( style != "" )
                    {
                        var remove = false;
                        if( style.substr( 0, 4 ) == "del " )
                        {
                            style = style.substr( 4 );
                            remove = true;
                        }
                        regex = RegExp( "([^" + revsol + "s:]+)(" + revsol + "s*:[^;]+)?", "i" );
                        const match = style.match( regex );
                        if( match )
                        {
                            const prop = match[ 1 ].trim();
                            regex = new RegExp( prop + revsol + "s*?:[^;]+;?", "gi" );

                            style_prop = style_prop.replace( regex, "" );

                            if( !remove )
                            {
                                const term = ( style_prop.slice( -1 ) != ";" ? ";" : "" );
                                style_prop += term + style;
                            }
                        }
                        else
                        {   console.error( "cascade was called with the following invalid css: " + style );
                        }
                    }
                });

                var curr_style = "";
                if( this.hasAttribute( "style" ))
                    curr_style = this.attributes[ "style" ].value;

                if( curr_style != style_prop )
                {
                    this.style = style_prop;

                    if( duration > 0 )
                    {   await new Promise( resolve => setTimeout( resolve, duration ));
                        this.style = old_style;
                    }
                }
            }
            else
            {   // css classes
                var classes_ = css_class_or_style.split( " " );

                var to_add = "+";
                var to_remove = "-";

                // loop twice if duration > 0
                while( true )
                {
                    classes_.forEach( class_ =>
                    {
                        var name = class_.trim().substr( 1 );
                        switch( class_.trim().charAt( 0 ))
                        {
                            case to_add:
                            {   this.classList.add( name );
                                break;
                            }
                            case to_remove:
                            {   this.classList.remove( name );
                                break;
                            }
                            default:
                            {   this.classList.toggle( class_ );
                            }
                        }
                    });

                    if( duration <= 0 )
                        break;
                    else
                    {
                        await new Promise( resolve => setTimeout( resolve, duration ));
                        duration = 0;
                        to_add = "-";
                        to_remove = "+";
                    }
                }
            }
            return this;
        };


        HTMLElement.prototype.cascade = mesis_cascade;

        HTMLElement.prototype.cascade_ = function( strings, ...values )
        {
            if( strings == undefined || strings.length == 0 )
                return;

            if( strings.lenght == 1 && strings[ 0 ] == "" )
                return;

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            this.cascade( res );
        };


        HTMLElement.prototype.css_p = function( css_prop )
        {
            const elem = this;
            if( css_prop.includes( "--" ))
            {
                const computedStyles = getComputedStyle( document.documentElement );
                var res = computedStyles.getPropertyValue( css_prop );
                if( res != "" ) return res;
            }
            if( "|left|top|width|height|".includes( css_prop ))
            {
                var rect = elem.getBoundingClientRect();
                switch( css_prop )
                {
                    case "left": {   return rect.left;  }
                    case "top": {   return rect.top;  }
                    case "width": {   return rect.width;  }
                    case "height": {   return rect.height;  }
                }
            }
            const computedStyles = getComputedStyle( elem );
            return computedStyles.getPropertyValue( css_prop );
        };


        HTMLElement.prototype.css_ = function( strings, ...values )
        {
            if( strings == undefined || strings.length == 0 )
                return;

            if( strings.lenght == 1 && strings[ 0 ] == "" )
                return;

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            return this.css_p( res );
        };


        /* css_link: no elemento atual, a propriedade css_prop sera sempre igual a mesma
                      no elemento indicado pelo seletor. Ela ainda vai acrescentar no
                      elemento atual uma data-css-[property] com o valor

                    2 sintaxes: prop, selector => elem.style.prop = selector.style.prop
                                prop, function => elem.style.prop = function()
        */
        HTMLElement.prototype.css_link = function( css_prop, selector, offset = null )
        {
            var elem = this;
            if( elem?._mesis_css_links == undefined )
                elem._mesis_css_links = {};

            syncCallback = function()
            {
                if( elem._mesis_css_links[ css_prop ] == undefined )
                    return;

                if( typeof selector == "function" )
                {
                    elem.style[ css_prop ] = selector();
                    requestAnimationFrame( elem._mesis_css_links[ css_prop ]);
                    return;
                }
                var tgt_elem = document.querySelector( selector );
                if( tgt_elem )
                {
                    if( "|left|top|width|height|".includes( css_prop ))
                    {
                        var rect = tgt_elem.getBoundingClientRect();
                        switch( css_prop )
                        {
                            case "left":
                            {   if( elem.style.left != ( rect.left + "px" ))
                                {   var val_ = rect.left;
                                    if( offset ) val_ += offset;
                                    elem.style.left = val_ + "px";
                                    elem.dataset[ "css_left" ] = val_.toString();
                                }
                                break;
                            }
                            case "top":
                            {   if( elem.style.top != ( rect.top + "px" ))
                                {   var val_ = rect.top;
                                    if( offset ) val_ += offset;
                                    elem.style.top = val_ + "px";
                                    elem.dataset[ "css_top" ] = val_.toString();
                                }
                                break;
                            }
                            case "width":
                            {   if( elem.style.width != ( rect.width + "px" ))
                                {   var val_ = rect.width;
                                    if( offset ) val_ += offset;
                                    elem.style.width = val_ + "px";
                                    elem.dataset[ "css_width" ] = val_.toString();
                                }
                                break;
                            }
                            case "height":
                            {   if( elem.style.height != ( rect.height + "px" ))
                                {   var val_ = rect.height;
                                    if( offset ) val_ += offset;
                                    elem.style.height = val_ + "px";
                                    elem.dataset[ "css_height" ] = val_.toString();
                                }
                                break;
                            }
                        }
                    }
                    else
                    {
                        const css_val = window.getComputedStyle( tgt_elem )[ css_prop ];
                        if( css_val && elem.style[ css_prop ] != css_val )
                        {   if( offset && !isNaN( parseFloat( css_val )))
                                css_val = ( parseFloat( css_val ) + offset );
                            if( offset && isNaN( parseFloat( css_val )))
                                css_val += offset;
                            elem.style[ css_prop ] = css_val;
                            elem.dataset[ "css_" + css_prop ] = css_val;
                        }
                    }

                    requestAnimationFrame( elem._mesis_css_links[ css_prop ]);
                }
                else
                {
                    console.error( "css_link could not find element for selector: " + selector );
                    console.log( "css_link removed" );
                    elem.css_unlink( css_prop );
                }
            };

            const boundCallback = syncCallback.bind( elem );
            elem._mesis_css_links[ css_prop ] = boundCallback;

            requestAnimationFrame( boundCallback );
        };


        HTMLElement.prototype.css_unlink = function( css_prop )
        {
            var elem = this;
            if(  elem?._mesis_css_links == undefined )
                return;
            if( css_prop in elem._mesis_css_links )
            {
                window.removeEventListener( 'scroll', elem._mesis_css_links[ css_prop ]);
                window.removeEventListener( 'resize', elem._mesis_css_links[ css_prop ]);
                delete elem._mesis_css_links[ css_prop ];
            }
        };


        Object.defineProperty( HTMLElement.prototype, 'text',
        {
            enumerable: true,
            configurable: true,

            get()
            {   return this.textContent   },

            set( new_text )
            {
                var text = new_text.toString();

                var node = null;
                for( let i = 0; i < this.childNodes.length; i++ )
                {
                    if( this.childNodes[ i ].nodeType === Node.TEXT_NODE )
                    {
                        node = this.childNodes[ i ];
                        break;
                    }
                }
                if( node )
                    node.nodeValue = new_text;
                else
                {   const newtxt = document.createTextNode( text );
                    this.prepend( newtxt );
                }
            }
        });


        to_JSON_string = function( target )
        {
            var res = "false";
            try{   res = JSON.stringify( target );}
            finally{}
            return res;
        };


        // a funcao underscore retorna o template string processado, mas na 1a ocorrencia
        // o adiciona a __mesis_ref_term_tokens. Numa 2a ocorrencia, retorna o token ja existente.
        // dessa forma termos repetidos passam a ser referencias aos declarados inicialmente
        _ = function( strings, ...values )
        {
            var MesisAgent = g_[ "MesisAgent" ];

            if( strings == undefined )
                return;

            let res = "";

            strings.forEach(( str, i ) =>
            {
                res += str.trim();
                if( i < values.length )
                    res += values[ i ].toString();
            });

            // como usamos o prefixo numerico _nnnnn__, temos q procurar por todos os termos
            // (a busca pelo termo em si falharia)
            let found = false;

            for( prop in __mesis_ref_term_tokens )
                if( prop.substr( prop.indexOf( "__" ) + 2 ) == res )
                {
                    res = prop;
                    found = true;
                    break;
                }

            if( found == false )
                // nao achou, e aqui ainda nao acrescentamos o prefixo _nnnnn__
                res = mesis_parse_tag( res );

            // aqui ou achamos ja com o prefixo _nnnnn__ ou ele foi acrescentado
            return res;
        };
    });

    val script_templ_v = g_[ "elem" ]( s( "[]:template" ));

    uint32_t lim = script_templ_v[ "length" ].as< uint32_t >();
    d_get( std::to_string( lim ), "templates encontrados" );

    bool mesis_found = false;

    val all_scripts = g_[ "Array" ].new_();
    for( uint32_t c = 0; c < lim; c++ )
    {
        val scripts = script_templ_v[ c ][ "content" ][ "children" ];
        int len = scripts[ "length" ].as< uint32_t >();

        for( uint32_t i = 0; i < len; i++ )
            if( s( scripts[ i ][ "tagName" ]) == s( "SCRIPT" ))
                all_scripts.call< void >( "push", scripts[ i ]);
    }

    lim = all_scripts[ "length" ].as< uint32_t >();
    val regexp = g_[ "RegExp" ].new_( s( R"-([\s]*Mesis[\s]*\[[\s]*_[\s]*`html)-" ));

    for( uint32_t c = 0; c < lim; c++ )
    {
        val script = all_scripts[ c ];

        string code = s( script[ "textContent" ]);
        d_get( code.substr( 0, 100 ), "codigo do script no template" );

        bool is_mesis = regexp.call< bool >( "test", code );
        if( is_mesis == false ) continue;

        // na pagina principal, no fluxo principal do Mesis, ele ignora qq script q nao seja
        // o main dentro de templates, e nao checa os delayed para executar o main. Somente
        // o main é executado para gerar HTML (nao se pode incluir uma ilha Mesis a posteriori
        // com o delayed). Apos a geracao do HTML scripts delayeds em templates sao executados,
        // mas nao se verifica se sao Mesis, apenas executa o JS
        mesis_found = true;

        val new_script = __copy_script( script );
        doc[ "head" ].call< void >( "appendChild", new_script );
        new_script.call< void >( "setAttribute", s( "data-mesis_main" ), s( "true" ));

        // cleanup: remove o script do template
        script.call< void >( "remove" );
        break;
    }

    if( mesis_found == false )
    {
        char btick = static_cast< char >( 96 );
        std::cerr << "Mesis didn't find the HTML generation script - nothing will be generated. Please add "
                  << "a TEMPLATE tag in the HEAD with a SCRIPT tag as the first content, with Mesis[_"
                  << btick << "html" << btick << "] = ... " << "declaration" << std::endl;
    }
    else html_src_v = g_[ "Mesis" ];

    emscripten_request_animation_frame_loop( Mesis_internal_loop, nullptr );
}


EMSCRIPTEN_BINDINGS( module )
{
    using namespace emscripten;

    function( "start",           &start );
    function( "fetch_island",    &fetch_island );
    function( "island_response", &island_response );
    function( "loop",                   &loop );
    function( "loop_s",                 &loop_s );
    function( "loop_off",               &loop_off );
    function( "loop_off_s",             &loop_off_s );
    function( "loop_dump",              &loop_dump );

    function( "Mesis_remove_from_loop", &Mesis_remove_from_loop );
    function( "Mesis_pause_on_loop",    &Mesis_pause_on_loop );
    function( "Mesis_restart_on_loop",  &Mesis_restart_on_loop );

    function( "kv_add",          &kv_add );
    function( "kv_add_jo",       &kv_add_jo );
    function( "kv_get",          &kv_get );
    function( "kv_del",          &kv_del );
    function( "kv_remove",       &kv_remove );
    function( "kv_search",       &kv_search );
    function( "set_cacheAPI",    &set_cacheAPI );
    function( "cache_overwrite", &cache_overwrite );
    function( "cache_remove",    &cache_remove );
    function( "cache_over_async",&cache_over_async );
    function( "cache_clone",     &cache_clone );
    function( "cache_clone_async",&cache_clone_async );

    // TODO: DEBUG: nao expor apos o DEBUG
    function( "stop_loop", &stop_loop );

}
