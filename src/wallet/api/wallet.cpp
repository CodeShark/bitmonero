// Copyright (c) 2014-2016, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers


#include "wallet.h"
#include "pending_transaction.h"
#include "transaction_history.h"
#include "common_defines.h"

#include "mnemonics/electrum-words.h"
#include <boost/format.hpp>
#include <sstream>

using namespace std;
using namespace cryptonote;

namespace Bitmonero {

namespace {
    // copy-pasted from simplewallet
    static const size_t DEFAULT_MIXIN = 4;
    static const int    DEFAULT_REFRESH_INTERVAL_SECONDS = 10;
}

struct Wallet2CallbackImpl : public tools::i_wallet2_callback
{

    Wallet2CallbackImpl()
     : m_listener(nullptr)
    {

    }

    ~Wallet2CallbackImpl()
    {

    }

    void setListener(WalletListener * listener)
    {
        m_listener = listener;
    }

    WalletListener * getListener() const
    {
        return m_listener;
    }

    virtual void on_new_block(uint64_t height, const cryptonote::block& block)
    {
        // TODO;
        LOG_PRINT_L3(__FUNCTION__ << ": new block. height: " << height);
    }

    virtual void on_money_received(uint64_t height, const cryptonote::transaction& tx, size_t out_index)
    {

        std::string tx_hash =  epee::string_tools::pod_to_hex(get_transaction_hash(tx));
        uint64_t amount     = tx.vout[out_index].amount;

        LOG_PRINT_L3(__FUNCTION__ << ": money received. height:  " << height
                     << ", tx: " << tx_hash
                     << ", amount: " << print_money(amount));
        if (m_listener) {
            m_listener->moneyReceived(tx_hash, amount);
            m_listener->updated();
        }
    }

    virtual void on_money_spent(uint64_t height, const cryptonote::transaction& in_tx, size_t out_index,
                                const cryptonote::transaction& spend_tx)
    {
        // TODO;
        std::string tx_hash = epee::string_tools::pod_to_hex(get_transaction_hash(spend_tx));
        uint64_t amount = in_tx.vout[out_index].amount;
        LOG_PRINT_L3(__FUNCTION__ << ": money spent. height:  " << height
                     << ", tx: " << tx_hash
                     << ", amount: " << print_money(amount));
        if (m_listener) {
            m_listener->moneySpent(tx_hash, amount);
            m_listener->updated();
        }
    }

    virtual void on_skip_transaction(uint64_t height, const cryptonote::transaction& tx)
    {
        // TODO;
    }

    WalletListener * m_listener;
};

Wallet::~Wallet() {}

WalletListener::~WalletListener() {}


string Wallet::displayAmount(uint64_t amount)
{
    return cryptonote::print_money(amount);
}

uint64_t Wallet::amountFromString(const string &amount)
{
    uint64_t result = 0;
    cryptonote::parse_amount(result, amount);
    return result;
}

uint64_t Wallet::amountFromDouble(double amount)
{
    std::stringstream ss;
    ss << std::fixed << std::setprecision(CRYPTONOTE_DISPLAY_DECIMAL_POINT) << amount;
    return amountFromString(ss.str());
}

std::string Wallet::genPaymentId()
{
    crypto::hash8 payment_id = crypto::rand<crypto::hash8>();
    return epee::string_tools::pod_to_hex(payment_id);

}

bool Wallet::paymentIdValid(const string &paiment_id)
{
    crypto::hash8 pid;
    return tools::wallet2::parse_short_payment_id(paiment_id, pid);
}

uint64_t Wallet::maximumAllowedAmount()
{
    return std::numeric_limits<uint64_t>::max();
}


///////////////////////// WalletImpl implementation ////////////////////////
WalletImpl::WalletImpl(bool testnet)
    :m_wallet(nullptr), m_status(Wallet::Status_Ok), m_trustedDaemon(false),
      m_wallet2Callback(nullptr)
{
    m_wallet = new tools::wallet2(testnet);
    m_history = new TransactionHistoryImpl(this);
    m_wallet2Callback = new Wallet2CallbackImpl;
    m_wallet->callback(m_wallet2Callback);
    m_refreshThreadDone = false;
    m_refreshEnabled = false;
    m_refreshIntervalSeconds = DEFAULT_REFRESH_INTERVAL_SECONDS;
    m_refreshThread = std::thread([this] () {
        this->refreshThreadFunc();
    });

}

WalletImpl::~WalletImpl()
{
    stopRefresh();
    delete m_history;
    delete m_wallet;
    delete m_wallet2Callback;
}

bool WalletImpl::create(const std::string &path, const std::string &password, const std::string &language)
{

    clearStatus();

    bool keys_file_exists;
    bool wallet_file_exists;
    tools::wallet2::wallet_exists(path, keys_file_exists, wallet_file_exists);
    // TODO: figure out how to setup logger;
    LOG_PRINT_L3("wallet_path: " << path << "");
    LOG_PRINT_L3("keys_file_exists: " << std::boolalpha << keys_file_exists << std::noboolalpha
                 << "  wallet_file_exists: " << std::boolalpha << wallet_file_exists << std::noboolalpha);


    // add logic to error out if new wallet requested but named wallet file exists
    if (keys_file_exists || wallet_file_exists) {
        m_errorString = "attempting to generate or restore wallet, but specified file(s) exist.  Exiting to not risk overwriting.";
        LOG_ERROR(m_errorString);
        m_status = Status_Error;
        return false;
    }
    // TODO: validate language
    m_wallet->set_seed_language(language);
    crypto::secret_key recovery_val, secret_key;
    try {
        recovery_val = m_wallet->generate(path, password, secret_key, false, false);
        m_password = password;
        m_status = Status_Ok;
    } catch (const std::exception &e) {
        LOG_ERROR("Error creating wallet: " << e.what());
        m_status = Status_Error;
        m_errorString = e.what();
        return false;
    }

    return true;
}

bool WalletImpl::open(const std::string &path, const std::string &password)
{
    clearStatus();
    try {
        // TODO: handle "deprecated"
        m_wallet->load(path, password);

        m_password = password;
    } catch (const std::exception &e) {
        LOG_ERROR("Error opening wallet: " << e.what());
        m_status = Status_Error;
        m_errorString = e.what();
    }
    return m_status == Status_Ok;
}

bool WalletImpl::recover(const std::string &path, const std::string &seed)
{
    clearStatus();
    m_errorString.clear();
    if (seed.empty()) {
        m_errorString = "Electrum seed is empty";
        LOG_ERROR(m_errorString);
        m_status = Status_Error;
        return false;
    }

    crypto::secret_key recovery_key;
    std::string old_language;
    if (!crypto::ElectrumWords::words_to_bytes(seed, recovery_key, old_language)) {
        m_errorString = "Electrum-style word list failed verification";
        m_status = Status_Error;
        return false;
    }

    try {
        m_wallet->set_seed_language(old_language);
        m_wallet->generate(path, "", recovery_key, true, false);
        // TODO: wallet->init(daemon_address);
    } catch (const std::exception &e) {
        m_status = Status_Error;
        m_errorString = e.what();
    }
    return m_status == Status_Ok;
}

bool WalletImpl::close()
{

    bool result = false;
    try {
        // do not store wallet with invalid status
        if (status() == Status_Ok)
            m_wallet->store();
        // LOG_PRINT_L0("wallet::store done");
        // LOG_PRINT_L0("Calling wallet::stop...");
        m_wallet->stop();
        // LOG_PRINT_L0("wallet::stop done");
        result = true;
        clearStatus();
    } catch (const std::exception &e) {
        m_status = Status_Error;
        m_errorString = e.what();
        LOG_ERROR("Error closing wallet: " << e.what());
    }
    return result;
}

std::string WalletImpl::seed() const
{
    std::string seed;
    if (m_wallet)
        m_wallet->get_seed(seed);
    return seed;
}

std::string WalletImpl::getSeedLanguage() const
{
    return m_wallet->get_seed_language();
}

void WalletImpl::setSeedLanguage(const std::string &arg)
{
    m_wallet->set_seed_language(arg);
}

int WalletImpl::status() const
{
    return m_status;
}

std::string WalletImpl::errorString() const
{
    return m_errorString;
}

bool WalletImpl::setPassword(const std::string &password)
{
    clearStatus();
    try {
        m_wallet->rewrite(m_wallet->get_wallet_file(), password);
        m_password = password;
    } catch (const std::exception &e) {
        m_status = Status_Error;
        m_errorString = e.what();
    }
    return m_status == Status_Ok;
}

std::string WalletImpl::address() const
{
    return m_wallet->get_account().get_public_address_str(m_wallet->testnet());
}

std::string WalletImpl::integratedAddress(const std::string &payment_id) const
{
    crypto::hash8 pid;
    if (!tools::wallet2::parse_short_payment_id(payment_id, pid)) {
        pid = crypto::rand<crypto::hash8>();
    }
    return m_wallet->get_account().get_public_integrated_address_str(pid, m_wallet->testnet());
}

bool WalletImpl::store(const std::string &path)
{
    clearStatus();
    try {
        if (path.empty()) {
            m_wallet->store();
        } else {
            m_wallet->store_to(path, m_password);
        }
    } catch (const std::exception &e) {
        LOG_ERROR("Error storing wallet: " << e.what());
        m_status = Status_Error;
        m_errorString = e.what();
    }

    return m_status == Status_Ok;
}

string WalletImpl::filename() const
{
    return m_wallet->get_wallet_file();
}

string WalletImpl::keysFilename() const
{
    return m_wallet->get_keys_file();
}

bool WalletImpl::init(const std::string &daemon_address, uint64_t upper_transaction_size_limit)
{
    clearStatus();

    m_wallet->init(daemon_address, upper_transaction_size_limit);
    if (Utils::isAddressLocal(daemon_address)) {
        this->setTrustedDaemon(true);
    }
    bool result = this->refresh();
    // enabling background refresh thread
    startRefresh();
    return result;

}

void WalletImpl::initAsync(const string &daemon_address, uint64_t upper_transaction_size_limit)
{
    clearStatus();
    m_wallet->init(daemon_address, upper_transaction_size_limit);
    if (Utils::isAddressLocal(daemon_address)) {
        this->setTrustedDaemon(true);
    }
    startRefresh();
}



uint64_t WalletImpl::balance() const
{
    return m_wallet->balance();
}

uint64_t WalletImpl::unlockedBalance() const
{
    return m_wallet->unlocked_balance();
}


bool WalletImpl::refresh()
{
    clearStatus();
    doRefresh();
    return m_status == Status_Ok;
}

void WalletImpl::refreshAsync()
{
    LOG_PRINT_L3(__FUNCTION__ << ": Refreshing asyncronously..");
    clearStatus();
    m_refreshCV.notify_one();
}

// TODO:
// 1 - properly handle payment id (add another menthod with explicit 'payment_id' param)
// 2 - check / design how "Transaction" can be single interface
// (instead of few different data structures within wallet2 implementation:
//    - pending_tx;
//    - transfer_details;
//    - payment_details;
//    - unconfirmed_transfer_details;
//    - confirmed_transfer_details)

PendingTransaction *WalletImpl::createTransaction(const string &dst_addr, const string &payment_id, uint64_t amount, uint32_t mixin_count,
                                                  PendingTransaction::Priority priority)

{
    clearStatus();
    vector<cryptonote::tx_destination_entry> dsts;
    cryptonote::tx_destination_entry de;

    // indicates if dst_addr is integrated address (address + payment_id)
    bool has_payment_id;
    crypto::hash8 payment_id_short;
    // TODO:  (https://bitcointalk.org/index.php?topic=753252.msg9985441#msg9985441)
    size_t fake_outs_count = mixin_count > 0 ? mixin_count : m_wallet->default_mixin();
    if (fake_outs_count == 0)
        fake_outs_count = DEFAULT_MIXIN;

    PendingTransactionImpl * transaction = new PendingTransactionImpl(*this);

    do {
        if(!cryptonote::get_account_integrated_address_from_str(de.addr, has_payment_id, payment_id_short, m_wallet->testnet(), dst_addr)) {
            // TODO: copy-paste 'if treating as an address fails, try as url' from simplewallet.cpp:1982
            m_status = Status_Error;
            m_errorString = "Invalid destination address";
            break;
        }


        std::vector<uint8_t> extra;
        // if dst_addr is not an integrated address, parse payment_id
        if (!has_payment_id && !payment_id.empty()) {
            // copy-pasted from simplewallet.cpp:2212
            crypto::hash payment_id_long;
            bool r = tools::wallet2::parse_long_payment_id(payment_id, payment_id_long);
            if (r) {
                std::string extra_nonce;
                cryptonote::set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id_long);
                r = add_extra_nonce_to_tx_extra(extra, extra_nonce);
            } else {
                r = tools::wallet2::parse_short_payment_id(payment_id, payment_id_short);
                if (r) {
                    std::string extra_nonce;
                    set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id_short);
                    r = add_extra_nonce_to_tx_extra(extra, extra_nonce);
                }
            }

            if (!r) {
                m_status = Status_Error;
                m_errorString = tr("payment id has invalid format, expected 16 or 64 character hex string: ") + payment_id;
                break;
            }
        }

        de.amount = amount;
        if (de.amount <= 0) {
            m_status = Status_Error;
            m_errorString = "Invalid amount";
            break;
        }

        dsts.push_back(de);
        //std::vector<tools::wallet2::pending_tx> ptx_vector;

        try {
            // priority called "fee_multiplied in terms of underlying wallet interface
            transaction->m_pending_tx = m_wallet->create_transactions_2(dsts, fake_outs_count, 0 /* unlock_time */,
                                                                      static_cast<uint64_t>(priority),
                                                                      extra, m_trustedDaemon);

        } catch (const tools::error::daemon_busy&) {
            // TODO: make it translatable with "tr"?
            m_errorString = tr("daemon is busy. Please try again later.");
            m_status = Status_Error;
        } catch (const tools::error::no_connection_to_daemon&) {
            m_errorString = tr("no connection to daemon. Please make sure daemon is running.");
            m_status = Status_Error;
        } catch (const tools::error::wallet_rpc_error& e) {
            m_errorString = tr("RPC error: ") +  e.to_string();
            m_status = Status_Error;
        } catch (const tools::error::get_random_outs_error&) {
            m_errorString = tr("failed to get random outputs to mix");
            m_status = Status_Error;

        } catch (const tools::error::not_enough_money& e) {
            m_status = Status_Error;
            std::ostringstream writer;

            writer << boost::format(tr("not enough money to transfer, available only %s, transaction amount %s = %s + %s (fee)")) %
                      print_money(e.available()) %
                      print_money(e.tx_amount() + e.fee())  %
                      print_money(e.tx_amount()) %
                      print_money(e.fee());
            m_errorString = writer.str();

        } catch (const tools::error::not_enough_outs_to_mix& e) {
            std::ostringstream writer;
            writer << tr("not enough outputs for specified mixin_count") << " = " << e.mixin_count() << ":";
            for (const std::pair<uint64_t, uint64_t> outs_for_amount : e.scanty_outs()) {
                writer << "\n" << tr("output amount") << " = " << print_money(outs_for_amount.first) << ", " << tr("found outputs to mix") << " = " << outs_for_amount.second;
            }
            m_errorString = writer.str();
            m_status = Status_Error;
        } catch (const tools::error::tx_not_constructed&) {
            m_errorString = tr("transaction was not constructed");
            m_status = Status_Error;
        } catch (const tools::error::tx_rejected& e) {
            std::ostringstream writer;
            writer << (boost::format(tr("transaction %s was rejected by daemon with status: ")) % get_transaction_hash(e.tx())) <<  e.status();
            m_errorString = writer.str();
            m_status = Status_Error;
        } catch (const tools::error::tx_sum_overflow& e) {
            m_errorString = e.what();
            m_status = Status_Error;
        } catch (const tools::error::zero_destination&) {
            m_errorString =  tr("one of destinations is zero");
            m_status = Status_Error;
        } catch (const tools::error::tx_too_big& e) {
            m_errorString =  tr("failed to find a suitable way to split transactions");
            m_status = Status_Error;
        } catch (const tools::error::transfer_error& e) {
            m_errorString = string(tr("unknown transfer error: ")) + e.what();
            m_status = Status_Error;
        } catch (const tools::error::wallet_internal_error& e) {
            m_errorString =  string(tr("internal error: ")) + e.what();
            m_status = Status_Error;
        } catch (const std::exception& e) {
            m_errorString =  string(tr("unexpected error: ")) + e.what();
            m_status = Status_Error;
        } catch (...) {
            m_errorString = tr("unknown error");
            m_status = Status_Error;
        }
    } while (false);

    transaction->m_status = m_status;
    transaction->m_errorString = m_errorString;
    return transaction;
}

void WalletImpl::disposeTransaction(PendingTransaction *t)
{
    delete t;
}

TransactionHistory *WalletImpl::history() const
{
    return m_history;
}

void WalletImpl::setListener(WalletListener *l)
{
    // TODO thread synchronization;
    m_wallet2Callback->setListener(l);
}

uint32_t WalletImpl::defaultMixin() const
{
    return m_wallet->default_mixin();
}

void WalletImpl::setDefaultMixin(uint32_t arg)
{
    m_wallet->default_mixin(arg);
}


bool WalletImpl::connectToDaemon()
{
    bool result = m_wallet->check_connection();
    m_status = result ? Status_Ok : Status_Error;
    if (!result) {
        m_errorString = "Error connecting to daemon at " + m_wallet->get_daemon_address();
    } else {
        // start refreshing here
    }
    return result;
}

bool WalletImpl::connected() const
{
    return m_wallet->check_connection();
}

void WalletImpl::setTrustedDaemon(bool arg)
{
    m_trustedDaemon = arg;
}

bool WalletImpl::trustedDaemon() const
{
    return m_trustedDaemon;
}

void WalletImpl::clearStatus()
{
    m_status = Status_Ok;
    m_errorString.clear();
}

void WalletImpl::refreshThreadFunc()
{
    LOG_PRINT_L3(__FUNCTION__ << ": starting refresh thread");

    while (true) {
        std::unique_lock<std::mutex> lock(m_refreshMutex);
        if (m_refreshThreadDone) {
            break;
        }
        LOG_PRINT_L3(__FUNCTION__ << ": waiting for refresh...");
        m_refreshCV.wait_for(lock, std::chrono::seconds(m_refreshIntervalSeconds));
        LOG_PRINT_L3(__FUNCTION__ << ": refresh lock acquired...");
        LOG_PRINT_L3(__FUNCTION__ << ": m_refreshEnabled: " << m_refreshEnabled);
        LOG_PRINT_L3(__FUNCTION__ << ": m_status: " << m_status);
        if (m_refreshEnabled /*&& m_status == Status_Ok*/) {
            LOG_PRINT_L3(__FUNCTION__ << ": refreshing...");
            doRefresh();
        }
    }
    LOG_PRINT_L3(__FUNCTION__ << ": refresh thread stopped");
}

void WalletImpl::doRefresh()
{
    // synchronizing async and sync refresh calls
    std::lock_guard<std::mutex> guarg(m_refreshMutex2);
    try {
        m_wallet->refresh();
    } catch (const std::exception &e) {
        m_status = Status_Error;
        m_errorString = e.what();
    }
    if (m_wallet2Callback->getListener()) {
        m_wallet2Callback->getListener()->refreshed();
    }
}


void WalletImpl::startRefresh()
{
    if (!m_refreshEnabled) {
        m_refreshEnabled = true;
        m_refreshCV.notify_one();
    }
}



void WalletImpl::stopRefresh()
{
    if (!m_refreshThreadDone) {
        m_refreshEnabled = false;
        m_refreshThreadDone = true;
        m_refreshThread.join();
    }
}

void WalletImpl::pauseRefresh()
{
    // TODO synchronize access
    if (!m_refreshThreadDone) {
        m_refreshEnabled = false;
    }
}


} // namespace
