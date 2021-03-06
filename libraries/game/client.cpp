#include <bts/blockchain/operation_factory.hpp>

#include <bts/game/rule_record.hpp>
#include <bts/game/game_operations.hpp>
#include <bts/game/client.hpp>

#include <bts/game/v8_api.hpp>
#include <bts/utilities/http_downloader.hpp>
#include <bts/game/v8_game.hpp>

#include <fc/io/json.hpp>
#include <fc/network/http/connection.hpp>
#include <fc/network/resolve.hpp>
#include <fc/network/url.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/thread/non_preemptable_scope_check.hpp>

#include <iostream>
#include <fstream>

namespace bts { namespace game {
   using namespace bts::blockchain;
   
   const operation_type_enum create_game_operation::type        = create_game_op_type;
   const operation_type_enum game_update_operation::type        = game_update_op_type;
   const operation_type_enum game_play_operation::type          = game_play_op_type;
    
    client* client::current = nullptr;
   
   namespace detail {
       class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
       public:
           virtual void* Allocate(size_t length) {
               void* data = AllocateUninitialized(length);
               return data == NULL ? data : memset(data, 0, length);
           }
           virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
           virtual void Free(void* data, size_t) { free(data); }
       };
       
      class client_impl {
          
      public:
         client*                 self;
         chain_database_ptr      _blockchain = nullptr;
         
         v8::Platform*           _platform;
          
          // For each game we should create a different isolate instance,
          // and do exit/dispose operation for each of them
          //
          // Here I just fixed the process hang issue.
          v8::Isolate* _isolate;
          
          ArrayBufferAllocator*  _allocator;
          
          exlib::Service*        _service;
         
         fc::path                _data_dir;
          
         std::unordered_map<std::string, v8_game_engine_ptr > _engines;
         
         boost::signals2::scoped_connection   _http_callback_signal_connection;
         
         client_impl(client* self)
         : self(self)
         {}
         ~client_impl(){
             // explicitly release enginer obj here
             // before we release isolate things.
             
             for(auto itr=_engines.begin(); itr!=_engines.end(); itr++)
             //for(auto e: _engines) not work???
             {
 
                 //int count = itr->second.use_count();
                 itr->second.reset();
             }
             
             _isolate->Exit();
             _isolate->Dispose();
             
             v8::V8::Dispose();
             v8::V8::ShutdownPlatform();
             delete _platform;
             delete _allocator;
         }
          
          static uint32_t* ComputeStackLimit(uint32_t size) {
              uint32_t* answer = &size - (size / sizeof(size));
              // If the size is very large and the stack is very near the bottom of
              // memory then the calculation above may wrap around and give an address
              // that is above the (downwards-growing) stack.  In that case we return
              // a very low address.
              if (answer > &size) return reinterpret_cast<uint32_t*>(sizeof(size));
              return answer;
          }
         
         void open(const fc::path& data_dir) {
            try {
               v8::V8::InitializeICU();
               exlib::Service::init();
                
               _platform = v8::platform::CreateDefaultPlatform();
               v8::V8::InitializePlatform(_platform);
               v8::V8::Initialize();
                
               
               _isolate = v8::Isolate::GetCurrent();
               if ( _isolate == NULL )
               {
                   /*
                   ResourceConstraints rc;
                   rc.set_max_old_space_size(10); //MB
                   rc.set_max_executable_size(10); //MB
                   
                   params.constraints.set_stack_limit(reinterpret_cast<uint32_t*>((char*)&rc - 1024 * 512));
                   https://github.com/v8/v8/blob/master/test/cctest/test-api.cc#L18724
                    */
                   _service = exlib::Service::current();
                   _allocator = new ArrayBufferAllocator();
                   Isolate::CreateParams create_params;
                   create_params.array_buffer_allocator = _allocator;
                   
                   ResourceConstraints rc;
                   rc.set_max_semi_space_size(40);
                   rc.set_max_old_space_size(60); //MB
                   rc.set_max_executable_size(60); //MB
                   static const int stack_breathing_room = 1024 * 1024;
//uint32_t* set_limit = ComputeStackLimit(stack_breathing_room);
                   rc.set_stack_limit(reinterpret_cast<uint32_t*>((char*)&rc - stack_breathing_room));
                   
                   create_params.constraints = rc;
                   
                   _isolate = v8::Isolate::New(create_params);
                   
                   
                   
                   //_isolate->SetStackLimit(reinterpret_cast<uintptr_t>(set_limit));
                   _isolate->Enter();
               }
                
               v8::V8::SetCaptureStackTraceForUncaughtExceptions(true, 10, StackTrace::kDetailed);
               
               ilog("Init class templat for game client" );
               
               v8_api::init_class_template( _isolate );

               
               // TODO: each rule instance is supposed to have their own context
               // TODO: To check whether the wallet and blockchain object are the same with the ones that should be used in script.
               bts::blockchain::operation_factory::instance().register_operation<create_game_operation>();
               bts::blockchain::operation_factory::instance().register_operation<game_update_operation>();
                
               bts::blockchain::operation_factory::instance().register_operation<game_play_operation>();
               
                /*
               game_executors::instance().register_game_executor(
                     std::function<void( chain_database_ptr, uint32_t, const pending_chain_state_ptr&)>(
                std::bind(&client::execute, self, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3 ) )
                                                                 );
                 */
               _http_callback_signal_connection =
               self->game_claimed_script.connect(
                                                 [=]( std::string code, std::string name) { this->script_http_callback( code, name ); } );
                
            } catch (...) {
            }
         }
          
          void   install_game_engine(const std::string& game_name, v8_game_engine_ptr engine_ptr )
          {
              FC_ASSERT( _engines.find( game_name ) == _engines.end(),
                        "Game Name already Registered ${name}", ("name", game_name) );
              
              if(engine_ptr != NULL)
              {
                  _engines[game_name] = engine_ptr;
              }
          }
          
          void install_game_engine_if_not(const std::string& game_name)
          {
              auto itr = _engines.find( game_name );
              
              if( itr == _engines.end() )
              {
                  try
                  {
                      install_game_engine(game_name, std::make_shared< v8_game_engine > (game_name, self));
                  } catch ( const fc::exception& e )
                  {
                      wlog("game engine register failed: ${x}", ("x",e.to_detail_string()));
                  }
              }
          }
         
         // Debuging file from operation and save to data_dir
          void script_http_callback( const std::string code, std::string game_name )
         {
            ilog("Storing the game code in a directory for viewing: ${game_name}", ("game_name", game_name) );
            fc::async( [=]()
                      {
                         
                         if ( !fc::exists(self->get_data_dir()) ) {
                            fc::create_directories(self->get_data_dir());
                         }
                         
                         ilog ("The data dir is ${d}", ("d", self->get_data_dir() ));
                         
                         fc::path script_filename = self->get_data_dir() / ( game_name + ".js");
                         
                         ilog ("The script_filename ${d}", ("d", script_filename ));
                         
                         std::ofstream script_file(script_filename.string());
                         
                         script_file << code;
                      }
                      );
         }
      };
   }
   
   
   client::client(chain_database_ptr chain)
   : my(new detail::client_impl(this))
   {
       my->_blockchain = chain;
   }
   
   client::~client()
   {
      close();
   }
   
   void client::open(const path& data_dir) {
       my->_data_dir = data_dir;
       my->open(data_dir);
       
       current = this;
   };
   
   fc::path client::get_data_dir()const
   {
      return my->_data_dir;
   }
    
    void client::execute( chain_database_ptr blockchain, uint32_t block_num, const pending_chain_state_ptr& pending_state )
    { try {
        wlog("Start executing in game client at block ${b}", ("b", block_num));
        auto games = blockchain->get_games("", -1);
        
        for ( const auto& g : games)
        {
            try {
                auto v8_game_engine = get_v8_engine( g.name );
                wlog("Start execute the game ${g}", ("g", g.name));
                v8_game_engine->execute( g.id, blockchain, block_num, pending_state );
            }
            catch (const game_engine_not_found& e)
            {
                wlog("game engine note found, failed to init for unknown reason during chain execution");
            }
        }
    } FC_CAPTURE_AND_RETHROW( (block_num) ) }
    
    chain_database_ptr client::get_chain_database()
    {
        return my->_blockchain;
    }
    
    v8_game_engine_ptr client::get_v8_engine(const std::string& game_name)
    {
        my->install_game_engine_if_not(game_name);
        
        auto itr = my->_engines.find( game_name );
        if( itr == my->_engines.end() )
            FC_THROW_EXCEPTION( bts::blockchain::game_engine_not_found, "", ("game_name", game_name) );
        return itr->second;
    }
    
    bool client::reinstall_game_engine(const std::string& game_name)
    {
        auto itr = my->_engines.find( game_name );
        if( itr != my->_engines.end() )
            my->_engines.erase(itr);
        
        get_v8_engine( game_name );
        
        return true;
    }
    
    client& client::get_current()
    {
        return *current;
    }
   
   void    client::close()
   {
      
   }
    
    void* client::get_isolate(/*const std::string& game_name*/)
    {
        return my->_isolate;
    }
    
    
} } // bts:game
