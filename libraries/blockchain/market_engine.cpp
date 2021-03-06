#include <bts/blockchain/exceptions.hpp>
#include <bts/blockchain/market_engine.hpp>
#include <fc/real128.hpp>
#include <algorithm>

namespace bts { namespace blockchain { namespace detail {

#define MARKET_ENGINE_PASS_PROCESS_MARGIN_CALLS   0
#define MARKET_ENGINE_PASS_PROCESS_EXPIRED_COVERS 1
#define MARKET_ENGINE_PASS_PROCESS_ASK_ORDERS     2
#define MARKET_ENGINE_PASS_COUNT                  3

  market_engine::market_engine( const pending_chain_state_ptr ps, const chain_database_impl& cdi )
  :_pending_state(ps),_db_impl(cdi)
  {
      _pending_state = std::make_shared<pending_chain_state>( ps );
      _prior_state = ps;
      //_eval_state._pending_state = _pending_state;
  }

  bool market_engine::execute( asset_id_type quote_id, asset_id_type base_id, const fc::time_point_sec timestamp )
  {
      try
      {
          _quote_id = quote_id;
          _base_id = base_id;

          oasset_record quote_asset = _pending_state->get_asset_record( _quote_id );
          oasset_record base_asset = _pending_state->get_asset_record( _base_id );

          FC_ASSERT( quote_asset.valid() && base_asset.valid() );
          FC_ASSERT( !quote_asset->flag_is_active( asset_record::halted_markets ) );
          FC_ASSERT( !base_asset->flag_is_active( asset_record::halted_markets ) );

          // The order book is sorted from low to high price. So to get the last item (highest bid),
          // we need to go to the first item in the next market class and then back up one
          const price current_pair = price( 0, quote_id, base_id );
          const price next_pair = (base_id+1 == quote_id) ? price( 0, quote_id+1, 0 ) : price( 0, quote_id, base_id+1 );

          _bid_itr           = _db_impl._bid_db.lower_bound( market_index_key( next_pair ) );

          _ask_itr           = _db_impl._ask_db.lower_bound( market_index_key( price( 0, quote_id, base_id ) ) );

          int last_orders_filled = -1;
          asset trading_volume(0, base_id);
          price opening_price, closing_price, highest_price, lowest_price;

          if( !_ask_itr.valid() ) _ask_itr = _db_impl._ask_db.begin();

          //
          // following logic depends on the internals of cached_level_map class.
          // if lower_bound() argument is past the end of the collection,
          // then result will point to end() which has valid() == false
          //
          // so we need to decrement the returned iterator, but
          // decrement operator may be undefined for end(), so we
          // special-case the result.
          //
          // one day we should write a greatest_element_below() method
          // for our DB class to implement the logic we're doing
          // directly here
          //
          // decrementing begin() OTOH is well-defined and returns
          // end() which is invalid, thus overflow of --itr here is
          // ok as long as we check valid() later
          //
          if( _bid_itr.valid() )   --_bid_itr;
          else _bid_itr = _db_impl._bid_db.last();

          // Market issued assets cannot match until the first time there is a median feed; assume feed price base id 0
          if( quote_asset->is_market_issued() && base_asset->id == 0 )
          {
              _feed_price = _db_impl.self->get_active_feed_price( _quote_id );
              const omarket_status market_stat = _pending_state->get_market_status( _quote_id, _base_id );
              if( (!market_stat.valid() || !market_stat->last_valid_feed_price.valid()) && !_feed_price.valid() )
                  FC_CAPTURE_AND_THROW( insufficient_feeds, (quote_id)(base_id) );
          }

          /*
          if( _feed_price.valid() )
             _short_at_limit_itr = std::set< pair<price,market_index_key> >::reverse_iterator(
                 _db_impl._short_limit_index.lower_bound(
                 std::make_pair( *_feed_price, market_index_key( current_pair ))
                 ));
           */

          _current_bid.reset();
          for( _current_pass=0; _current_pass<MARKET_ENGINE_PASS_COUNT; _current_pass++ )
          {
           _current_ask.reset();
           while( true )
           {
            if( (!_current_bid.valid()) || (_current_bid->get_balance().amount <= 0) )
            {
                ilog("getting next bid");
                get_next_bid();
                if( (!_current_bid.valid()) )
                {
                    ilog("market engine terminating due to no more bids");
                    break;
                }
                idump( (_current_bid) );
            }
            if( (!_current_ask.valid()) || (_current_ask->get_balance().amount <= 0) )
            {
                ilog("getting next ask");
                get_next_ask();
                if( (!_current_ask.valid()) )
                {
                    ilog("market engine terminating due to no more asks");
                    break;
                }
                idump( (_current_ask) );
            }

            // Make sure that at least one order was matched every time we enter the loop
            FC_ASSERT( _orders_filled != last_orders_filled, "We appear caught in an order matching loop!" );
            last_orders_filled = _orders_filled;

            // Initialize the market transaction
            market_transaction mtrx;
            mtrx.bid_owner = _current_bid->get_owner();
            mtrx.ask_owner = _current_ask->get_owner();
            if( _feed_price )
            {
               mtrx.bid_price = _current_bid->get_price( *_feed_price );
               mtrx.ask_price = _current_ask->get_price( *_feed_price );
            }
            else
            {
               mtrx.bid_price = _current_bid->get_price();
               mtrx.ask_price = _current_ask->get_price();
            }
            mtrx.bid_type  = _current_bid->type;
            mtrx.ask_type  = _current_ask->type;

            if( mtrx.bid_price < mtrx.ask_price )
            {
               wlog( "bid_price ${b} < ask_price ${a}; exit market loop", ("b",mtrx.bid_price)("a",mtrx.ask_price) );
               break;
            }

            if( _current_ask->type == ask_order && _current_bid->type == bid_order )
            {
                const asset bid_quantity_xts = _current_bid->get_quantity( _feed_price ? *_feed_price : price() );
                const asset ask_quantity_xts = _current_ask->get_quantity( _feed_price ? *_feed_price : price() );
                const asset quantity_xts = std::min( bid_quantity_xts, ask_quantity_xts );

                // Everyone gets the price they asked for
                mtrx.ask_received   = quantity_xts * mtrx.ask_price;
                mtrx.bid_paid       = quantity_xts * mtrx.bid_price;

                mtrx.ask_paid       = quantity_xts;
                mtrx.bid_received   = quantity_xts;

                // Handle rounding errors
                if( quantity_xts == bid_quantity_xts )
                   mtrx.bid_paid = _current_bid->get_balance();

                if( quantity_xts == ask_quantity_xts )
                   mtrx.ask_paid = _current_ask->get_balance();

                mtrx.quote_fees = mtrx.bid_paid - mtrx.ask_received;

                pay_current_bid( mtrx, *base_asset, *quote_asset );
                pay_current_ask( mtrx, *base_asset, *quote_asset );
            }

            push_market_transaction( mtrx );

            quote_asset->collected_fees += mtrx.quote_fees.amount;
            base_asset->collected_fees += mtrx.base_fees.amount;

            if( mtrx.ask_received.asset_id == 0 )
              trading_volume += mtrx.ask_received;
            else if( mtrx.bid_received.asset_id == 0 )
              trading_volume += mtrx.bid_received;
            if( opening_price == price() )
              opening_price = mtrx.bid_price;
            closing_price = mtrx.bid_price;
            //
            // Remark: only prices of matched orders are used to update market history
            //
            // Because of prioritization, we need the second comparison
            // in the following if statements.  Ask-side orders are
            // only sorted by price within a single pass.
            //
            if( highest_price == price() || highest_price < mtrx.bid_price)
              highest_price = mtrx.bid_price;
            // TODO check here: store lowest ask price or lowest bid price?
            if( lowest_price == price() || lowest_price > mtrx.ask_price)
              lowest_price = mtrx.ask_price;
           }
          }

          // update any fees collected
          _pending_state->store_asset_record( *quote_asset );
          _pending_state->store_asset_record( *base_asset );

          // Update market status and market history
          {
              omarket_status market_stat = _pending_state->get_market_status( _quote_id, _base_id );
              if( !market_stat.valid() ) market_stat = market_status( _quote_id, _base_id );
              market_stat->last_error.reset();
              _pending_state->store_market_status( *market_stat );

              // Remark: only prices of matched orders be updated to market history
              update_market_history( trading_volume, highest_price, lowest_price, opening_price, closing_price, timestamp );
          }

          //_eval_state.update_delegate_votes();
          _pending_state->apply_changes();
          return true;
    }
    catch( const fc::exception& e )
    {
        wlog( "error executing market ${quote} / ${base}\n ${e}", ("quote",quote_id)("base",base_id)("e",e.to_detail_string()) );
        omarket_status market_stat = _prior_state->get_market_status( _quote_id, _base_id );
        if( !market_stat.valid() ) market_stat = market_status( _quote_id, _base_id );
        market_stat->last_error = e;
        _prior_state->store_market_status( *market_stat );
    }
    return false;
  } // execute(...)

  void market_engine::push_market_transaction( const market_transaction& mtrx )
  { try {
      // If not an automatic market cancel
      if( mtrx.ask_paid.amount != 0
          || mtrx.ask_received.amount != 0
          || mtrx.bid_received.asset_id != 0
          || mtrx.bid_paid.amount != 0 )
      {
          FC_ASSERT( mtrx.bid_paid.amount >= 0 );
          FC_ASSERT( mtrx.ask_paid.amount >= 0 );
          FC_ASSERT( mtrx.bid_received.amount >= 0 );
          FC_ASSERT( mtrx.ask_received.amount>= 0 );
          FC_ASSERT( mtrx.bid_paid >= mtrx.ask_received );
          FC_ASSERT( mtrx.ask_paid >= mtrx.bid_received );
          FC_ASSERT( mtrx.quote_fees.amount >= 0 );
          FC_ASSERT( mtrx.base_fees.amount >= 0 );
      }

      _market_transactions.push_back(mtrx);
  } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

  void market_engine::pay_current_bid( market_transaction& mtrx, asset_record& base_asset, asset_record& quote_asset )
  { try {
      FC_ASSERT( _current_bid->type == bid_order );
      FC_ASSERT( mtrx.bid_type == bid_order );

      _current_bid->state.balance -= mtrx.bid_paid.amount;
      FC_ASSERT( _current_bid->state.balance >= 0 );

      FC_ASSERT( base_asset.address_is_whitelisted( mtrx.bid_owner ) );

      auto bid_payout = _pending_state->get_balance_record(
                                withdraw_condition( withdraw_with_signature(mtrx.bid_owner), _base_id ).get_address() );
      if( !bid_payout )
          bid_payout = balance_record( mtrx.bid_owner, asset(0,_base_id), 0 );

      share_type issuer_fee = 0;
      if( base_asset.market_fee_rate > 0 )
      {
          FC_ASSERT( base_asset.market_fee_rate <= BTS_BLOCKCHAIN_MAX_UIA_MARKET_FEE_RATE );
          issuer_fee = mtrx.bid_received.amount * base_asset.market_fee_rate;
          issuer_fee /= BTS_BLOCKCHAIN_MAX_UIA_MARKET_FEE_RATE;
      }

      mtrx.base_fees.amount += issuer_fee;
      mtrx.bid_received.amount -= issuer_fee;

      bid_payout->balance += mtrx.bid_received.amount;
      bid_payout->last_update = _pending_state->now();
      bid_payout->deposit_date = _pending_state->now();
      _pending_state->store_balance_record( *bid_payout );

      // if the balance is less than 1 XTS then it gets collected as fees.
      if( (_current_bid->get_quote_quantity() * mtrx.bid_price).amount == 0 )
      {
          mtrx.quote_fees.amount += _current_bid->state.balance;
          _current_bid->state.balance = 0;
      }
      _pending_state->store_bid_record( _current_bid->market_index, _current_bid->state );
  } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

  void market_engine::pay_current_ask( market_transaction& mtrx, asset_record& base_asset, asset_record& quote_asset )
  { try {
      FC_ASSERT( _current_ask->type == ask_order );
      FC_ASSERT( mtrx.ask_type == ask_order );

      _current_ask->state.balance -= mtrx.ask_paid.amount;
      FC_ASSERT( _current_ask->state.balance >= 0, "balance: ${b}", ("b",_current_ask->state.balance) );

      FC_ASSERT( quote_asset.address_is_whitelisted( mtrx.ask_owner ) );

      auto ask_balance_address = withdraw_condition( withdraw_with_signature(mtrx.ask_owner), _quote_id ).get_address();
      auto ask_payout = _pending_state->get_balance_record( ask_balance_address );
      if( !ask_payout )
          ask_payout = balance_record( mtrx.ask_owner, asset(0,_quote_id), 0 );

      share_type issuer_fee = 0;
      if( quote_asset.market_fee_rate > 0 )
      {
          FC_ASSERT( quote_asset.market_fee_rate <= BTS_BLOCKCHAIN_MAX_UIA_MARKET_FEE_RATE );
          issuer_fee = mtrx.ask_received.amount * quote_asset.market_fee_rate;
          issuer_fee /= BTS_BLOCKCHAIN_MAX_UIA_MARKET_FEE_RATE;
      }

      mtrx.quote_fees.amount += issuer_fee;
      mtrx.ask_received.amount -= issuer_fee;

      ask_payout->balance += mtrx.ask_received.amount;
      ask_payout->last_update = _pending_state->now();
      ask_payout->deposit_date = _pending_state->now();

      _pending_state->store_balance_record( *ask_payout );

      // if the balance is less than 1 XTS * PRICE < .001 USD XTS goes to fees
      if( (_current_ask->get_quantity() * mtrx.ask_price).amount == 0 )
      {
          mtrx.base_fees.amount += _current_ask->state.balance;
          _current_ask->state.balance = 0;
      }

      _pending_state->store_ask_record( _current_ask->market_index, _current_ask->state );

  } FC_CAPTURE_AND_RETHROW( (mtrx) )  } // pay_current_ask

  /**
   *  if there are bids above feed price, take the max of the two
   *  if there are shorts at the feed take the next short
   *  if there are bids with prices above the limit of the current short they should take priority
   *       - shorts need to be ordered by limit first, then interest rate *WHEN* the limit is
   *         lower than the feed price.
   */
  bool market_engine::get_next_bid()
  { try {
      if( _current_bid && _current_bid->get_quantity().amount > 0 )
        return _current_bid.valid();

      ++_orders_filled;
      _current_bid.reset();

      optional<market_order> bid;

      if( _bid_itr.valid() )
      {
         market_order abs_bid( bid_order, _bid_itr.key(), _bid_itr.value() );
         if( abs_bid.get_price().quote_asset_id == _quote_id && abs_bid.get_price().base_asset_id == _base_id )
         {
            bid = abs_bid;
         }
      }
      if( !_feed_price )
      {
         _current_bid = bid;
         --_bid_itr;
         return _current_bid.valid();
      }

      /*
      // if bids are less than feed price, then we can consider shorts
      if( !bid || (bid->get_price(*_feed_price) < *_feed_price) )
      {
          // first consider shorts at the feed price
          if( _short_at_feed_itr != _db_impl._shorts_at_feed.rend() )
          {
              // if we skipped past the range for our market, note that we reached the end
              if( _short_at_feed_itr->order_price.quote_asset_id != _quote_id ||
                  _short_at_feed_itr->order_price.base_asset_id != _base_id  )
              {
                 _short_at_feed_itr = _db_impl._shorts_at_feed.rend();
              }
              else // fetch the order
              {
                 optional<order_record> oshort = _pending_state->get_short_record( *_short_at_feed_itr );
                 FC_ASSERT( oshort.valid() );
                 bid =  market_order( short_order, *_short_at_feed_itr, *oshort );
                 _current_bid = bid;
                 ++_short_at_feed_itr;
                 return _current_bid.valid();
              }
          }
          else
          {
             wlog( "feed at short itr is null" );
          }

          // if we have a short with a limit below the feed
          if( _short_at_limit_itr != _db_impl._short_limit_index.rend() )
          {
             //wdump((*_short_at_limit_itr) );
             if( _short_at_limit_itr->first.quote_asset_id != _quote_id ||
                 _short_at_limit_itr->first.base_asset_id != _base_id  )
             {
                _short_at_limit_itr = _db_impl._short_limit_index.rend();
             }
             else
             {
                // if the limit price is better than a current bid
                if( !bid || (_short_at_limit_itr->first > bid->get_price(*_feed_price)) )
                {
                   // then the short is game.
                   optional<order_record> oshort = _pending_state->get_short_record( _short_at_limit_itr->second );
                   FC_ASSERT( oshort );
                   bid =  market_order( short_order, _short_at_limit_itr->second, *oshort );
                   _current_bid = bid;
                   ++_short_at_limit_itr;
                   return _current_bid.valid();
                }
             }
          }
          // then consider shorts by limit
      }
    */

      if( bid )
      {
          _current_bid = bid;
          switch( uint8_t(bid->type) )
          {
              case bid_order:
                  --_bid_itr;
                  break;
              default:
                  FC_ASSERT( false, "Unknown Bid Type" );
          }
      }

      return _current_bid.valid();
  } FC_CAPTURE_AND_RETHROW() }

  bool market_engine::get_next_ask()
  { try {
      if( _current_ask && _current_ask->state.balance > 0 )
          return _current_ask.valid();
      
      _current_ask.reset();
      ++_orders_filled;

      switch( _current_pass )
      {
      case MARKET_ENGINE_PASS_PROCESS_ASK_ORDERS:
          return get_next_ask_order();
      default:
          FC_ASSERT( false, "_current_pass value is unknown" );
      }
      return false;
      } FC_CAPTURE_AND_RETHROW()
  }

  bool market_engine::get_next_ask_order()
  {
      /**
       *  Process asks.
       */

      if( !_ask_itr.valid() )
          return false;

      market_order abs_ask = market_order( ask_order, _ask_itr.key(), _ask_itr.value() );
      if( (abs_ask.get_price().quote_asset_id != _quote_id) || (abs_ask.get_price().base_asset_id != _base_id) )
          return false;

      _current_ask = abs_ask;
      ++_ask_itr;

      return true;
  }

  /**
    *  This method should not affect market execution or validation and
    *  is for historical purposes only.
    */
  void market_engine::update_market_history( const asset& trading_volume,
                                             const price& highest_price,
                                             const price& lowest_price,
                                             const price& opening_price,
                                             const price& closing_price,
                                             const fc::time_point_sec timestamp )
  {
          // Remark: only prices of matched orders be updated to market history
          if( trading_volume.amount > 0 )
          {
            market_history_key key(_quote_id, _base_id, market_history_key::each_block, _db_impl._head_block_header.timestamp);
            market_history_record new_record(highest_price,
                                            lowest_price,
                                            opening_price,
                                            closing_price,
                                            trading_volume.amount);

            //LevelDB iterators are dumb and don't support proper past-the-end semantics.
            auto last_key_itr = _db_impl._market_history_db.lower_bound(key);
            if( !last_key_itr.valid() )
              last_key_itr = _db_impl._market_history_db.last();
            else
              --last_key_itr;

            key.timestamp = timestamp;

            //Unless the previous record for this market is the same as ours...
            // TODO check here: the previous commit checks for volume and prices change here,
            //                  I replaced them with key comparison, but looks odd as well.
            //                  maybe need to remove the judgements at all? since volume info is
            //                  always needed to be updated to market history,
            //                  even if prices and volumes are same to last block.
            if( (!(last_key_itr.valid()
                && last_key_itr.key() == key)) )
            {
              //...add a new entry to the history table.
              _pending_state->market_history[key] = new_record;
            }

            fc::time_point_sec start_of_this_hour = timestamp - (timestamp.sec_since_epoch() % (60*60));
            market_history_key old_key(_quote_id, _base_id, market_history_key::each_hour, start_of_this_hour);
            if( auto opt = _db_impl._market_history_db.fetch_optional(old_key) )
            {
              auto old_record = *opt;
              old_record.volume += new_record.volume;
              old_record.closing_price = new_record.closing_price;
              if( new_record.highest_bid > old_record.highest_bid || new_record.lowest_ask < old_record.lowest_ask )
              {
                old_record.highest_bid = std::max(new_record.highest_bid, old_record.highest_bid);
                old_record.lowest_ask = std::min(new_record.lowest_ask, old_record.lowest_ask);
              }
              // always update old data since volume changed
              _pending_state->market_history[old_key] = old_record;
            }
            else
              _pending_state->market_history[old_key] = new_record;

            fc::time_point_sec start_of_this_day = timestamp - (timestamp.sec_since_epoch() % (60*60*24));
            old_key = market_history_key(_quote_id, _base_id, market_history_key::each_day, start_of_this_day);
            if( auto opt = _db_impl._market_history_db.fetch_optional(old_key) )
            {
              auto old_record = *opt;
              old_record.volume += new_record.volume;
              old_record.closing_price = new_record.closing_price;
              if( new_record.highest_bid > old_record.highest_bid || new_record.lowest_ask < old_record.lowest_ask )
              {
                old_record.highest_bid = std::max(new_record.highest_bid, old_record.highest_bid);
                old_record.lowest_ask = std::min(new_record.lowest_ask, old_record.lowest_ask);
              }
              // always update old data since volume changed
              _pending_state->market_history[old_key] = old_record;
            }
            else
              _pending_state->market_history[old_key] = new_record;
          }
  }

  asset market_engine::get_interest_paid(const asset& total_amount_paid, const price& apr, uint32_t age_seconds)
  {
      // TOTAL_PAID = DELTA_PRINCIPLE + DELTA_PRINCIPLE * APR * PERCENT_OF_YEAR
      // DELTA_PRINCIPLE = TOTAL_PAID / (1 + APR*PERCENT_OF_YEAR)
      // INTEREST_PAID  = TOTAL_PAID - DELTA_PRINCIPLE
      fc::real128 total_paid( total_amount_paid.amount );
      fc::real128 apr_n( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) * apr).amount );
      fc::real128 apr_d( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) ).amount );
      fc::real128 iapr = apr_n / apr_d;
      fc::real128 age_sec(age_seconds);
      fc::real128 sec_per_year(365 * 24 * 60 * 60);
      fc::real128 percent_of_year = age_sec / sec_per_year;

      fc::real128 delta_principle = total_paid / ( fc::real128(1) + iapr * percent_of_year );
      fc::real128 interest_paid   = total_paid - delta_principle;

      return asset( interest_paid.to_uint64(), total_amount_paid.asset_id );
  }

  asset market_engine::get_interest_owed(const asset& principle, const price& apr, uint32_t age_seconds)
  {
      // INTEREST_OWED = TOTAL_PRINCIPLE * APR * PERCENT_OF_YEAR
      fc::real128 total_principle( principle.amount );
      fc::real128 apr_n( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) * apr).amount );
      fc::real128 apr_d( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) ).amount );
      fc::real128 iapr = apr_n / apr_d;
      fc::real128 age_sec(age_seconds);
      fc::real128 sec_per_year(365 * 24 * 60 * 60);
      fc::real128 percent_of_year = age_sec / sec_per_year;

      fc::real128 interest_owed   = total_principle * iapr * percent_of_year;

      return asset( interest_owed.to_uint64(), principle.asset_id );
  }

} } } // end namespace bts::blockchain::detail
