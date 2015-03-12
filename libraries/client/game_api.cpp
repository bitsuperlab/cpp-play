#include <bts/client/client_impl.hpp>

using namespace bts::client;
using namespace bts::client::detail;

wallet_transaction_record client_impl::game_buy_chips(
        const string& from_account,
        double quantity, 
        const string& quantity_symbol )
{
    auto record = _wallet->buy_chips(from_account, quantity, quantity_symbol, true);
    _wallet->cache_transaction(record);
    network_broadcast_transaction(record.trx);
    return record;
}

wallet_transaction_record client_impl::game_create(
                                                                  const std::string& symbol,
                                                                  const std::string& game_name,
                                                                  const std::string& issuer_name,
                                                                  const std::string& asset_symbol,
                                                                  const std::string& script_url,
                                                                  const std::string& script_hash,
                                                                  const std::string& description,
                                                                  const fc::variant& public_data /* = fc::json::from_string("null").as<fc::variant>() */ )
{
   auto record = _wallet->create_game( symbol, game_name, description, public_data, issuer_name,
                                            asset_symbol, script_url, script_hash, true );
   _wallet->cache_transaction( record );
   network_broadcast_transaction( record.trx );
   return record;
}

wallet_transaction_record client_impl::game_play(const std::string& asset_symbol, const fc::variant& param )
{
    auto record = _wallet->play_game(asset_symbol, param, true);
    _wallet->cache_transaction(record);
    network_broadcast_transaction(record.trx);
    return record;
}
