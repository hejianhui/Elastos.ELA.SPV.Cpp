// Copyright (c) 2012-2018 The Elastos Open Source Project
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stdlib.h>
#include <boost/scoped_ptr.hpp>
#include <Core/BRTransaction.h>
#include <SDK/ELACoreExt/ELATxOutput.h>
#include <boost/function.hpp>
#include <SDK/Common/Log.h>
#include <Core/BRAddress.h>
#include <Core/BRBIP32Sequence.h>
#include <Core/BRWallet.h>
#include <SDK/Transaction/Transaction.h>
#include <SDK/Common/ParamChecker.h>

#include "BRAddress.h"
#include "BRBIP39Mnemonic.h"
#include "BRArray.h"
#include "BRTransaction.h"

#include "Wallet.h"
#include "Utils.h"
#include "ELACoreExt/ELATransaction.h"
#include "ELATxOutput.h"
#include "Account/MultiSignSubAccount.h"

namespace Elastos {
	namespace ElaWallet {

		typedef struct UTXO {
			BRUTXO o;
			uint64_t amount;
		} UTXO_t;

		ELAWallet *ELAWalletNew(BRTransaction *transactions[], size_t txCount,
								size_t (*WalletUnusedAddrs)(BRWallet *wallet, BRAddress addrs[], uint32_t gapLimit,
															int internal),
								size_t (*WalletAllAddrs)(BRWallet *wallet, BRAddress addrs[], size_t addrsCount),
								void (*setApplyFreeTx)(void *info, void *tx),
								void (*WalletUpdateBalance)(BRWallet *wallet),
								int (*WalletContainsTx)(BRWallet *wallet, const BRTransaction *tx),
								void (*WalletAddUsedAddrs)(BRWallet *wallet, const BRTransaction *tx),
								BRTransaction *(*WalletCreateTxForOutputs)(BRWallet *wallet,
																		   const BRTxOutput outputs[],
																		   size_t outCount),
								uint64_t (*WalletMaxOutputAmount)(BRWallet *wallet),
								uint64_t (*WalletFeeForTx)(BRWallet *wallet, const BRTransaction *tx),
								int (*TransactionIsSigned)(const BRTransaction *tx),
								size_t (*KeyToAddress)(const BRKey *key, char *addr, size_t addrLen),
								uint64_t (*balanceAfterTx)(BRWallet *wallet, const BRTransaction *tx)) {
			ELAWallet *wallet = NULL;
			BRTransaction *tx;

			assert(transactions != NULL || txCount == 0);
			wallet = new ELAWallet();
			assert(wallet != NULL);
			array_new(wallet->Raw.utxos, 100);
			array_new(wallet->Raw.transactions, txCount + 100);
			wallet->Raw.feePerKb = DEFAULT_FEE_PER_KB;
			wallet->Raw.lastBlockHeight = 0;
			wallet->Raw.WalletUnusedAddrs = WalletUnusedAddrs;
			wallet->Raw.WalletAllAddrs = WalletAllAddrs;
			wallet->Raw.setApplyFreeTx = setApplyFreeTx;
			wallet->Raw.WalletUpdateBalance = WalletUpdateBalance;
			wallet->Raw.WalletContainsTx = WalletContainsTx;
			wallet->Raw.WalletAddUsedAddrs = WalletAddUsedAddrs;
			wallet->Raw.WalletCreateTxForOutputs = WalletCreateTxForOutputs;
			wallet->Raw.WalletMaxOutputAmount = WalletMaxOutputAmount;
			wallet->Raw.WalletFeeForTx = WalletFeeForTx;
			wallet->Raw.TransactionIsSigned = TransactionIsSigned;
			wallet->Raw.KeyToAddress = KeyToAddress;
			wallet->Raw.balanceAfterTx = balanceAfterTx;
			array_new(wallet->Raw.internalChain, 100);
			array_new(wallet->Raw.externalChain, 100);
			array_new(wallet->Raw.balanceHist, txCount + 100);
			wallet->Raw.allTx = BRSetNew(BRTransactionHash, BRTransactionEq, txCount + 100);
			wallet->Raw.invalidTx = BRSetNew(BRTransactionHash, BRTransactionEq, 10);
			wallet->Raw.pendingTx = BRSetNew(BRTransactionHash, BRTransactionEq, 10);
			wallet->Raw.spentOutputs = BRSetNew(BRUTXOHash, BRUTXOEq, txCount + 100);
			wallet->Raw.usedAddrs = BRSetNew(BRAddressHash, BRAddressEq, txCount + 100);
			wallet->Raw.allAddrs = BRSetNew(BRAddressHash, BRAddressEq, txCount + 100);
			wallet->TxRemarkMap = ELAWallet::TransactionRemarkMap();
			wallet->ListeningAddrs = std::vector<std::string>();
			pthread_mutex_init(&wallet->Raw.lock, NULL);

			for (size_t i = 0; transactions && i < txCount; i++) {
				tx = transactions[i];
				if (!wallet->Raw.TransactionIsSigned(tx) || BRSetContains(wallet->Raw.allTx, tx)) continue;
				BRSetAdd(wallet->Raw.allTx, tx);
				_BRWalletInsertTx((BRWallet *) wallet, tx);
			}

			return wallet;
		}

		void ELAWalletFree(ELAWallet *wallet, bool freeInternal) {
			assert(wallet != NULL);
			pthread_mutex_lock(&wallet->Raw.lock);
			BRSetFree(wallet->Raw.allAddrs);
			BRSetFree(wallet->Raw.usedAddrs);
			BRSetFree(wallet->Raw.invalidTx);
			BRSetFree(wallet->Raw.pendingTx);
			BRSetApply(wallet->Raw.allTx, NULL, wallet->Raw.setApplyFreeTx);
			BRSetFree(wallet->Raw.allTx);
			BRSetFree(wallet->Raw.spentOutputs);
			if (freeInternal)
				array_free(wallet->Raw.internalChain);
			array_free(wallet->Raw.externalChain);
			array_free(wallet->Raw.balanceHist);
			array_free(wallet->Raw.transactions);
			array_free(wallet->Raw.utxos);
			pthread_mutex_unlock(&wallet->Raw.lock);
			pthread_mutex_destroy(&wallet->Raw.lock);

			delete wallet;
		}

		std::string ELAWalletGetRemark(ELAWallet *wallet, const std::string &txHash) {
			if (wallet->TxRemarkMap.find(txHash) == wallet->TxRemarkMap.end())
				return "";
			return wallet->TxRemarkMap[txHash];
		}

		void ELAWalletRegisterRemark(ELAWallet *wallet, const std::string &txHash,
									 const std::string &remark) {
			wallet->TxRemarkMap[txHash] = remark;
		}

		void ELAWalletLoadRemarks(ELAWallet *wallet,
								  const SharedWrapperList<Transaction, BRTransaction *> &transaction) {
			for (int i = 0; i < transaction.size(); ++i) {
				wallet->TxRemarkMap[Utils::UInt256ToString(transaction[i]->getHash(), true)] =
					((ELATransaction *) transaction[i]->getRaw())->Remark;
			}
		}

		int UTXOCompareAscending(const void *o1, const void *o2) {
			if (((const UTXO_t *)o1)->amount > ((const UTXO_t *)o2)->amount) return 1;
			if (((const UTXO_t *)o1)->amount < ((const UTXO_t *)o2)->amount) return -1;
			return 0;
		}

		int UTXOCompareDescending(const void *o1, const void *o2) {
			if (((const UTXO_t *)o1)->amount < ((const UTXO_t *)o2)->amount) return 1;
			if (((const UTXO_t *)o1)->amount > ((const UTXO_t *)o2)->amount) return -1;
			return 0;
		}

		Wallet::Wallet() {

		}

		Wallet::Wallet(const SharedWrapperList<Transaction, BRTransaction *> &transactions,
					   const SubAccountPtr &subAccount,
					   const boost::shared_ptr<Listener> &listener) :
			_subAccount(subAccount) {

			_wallet = ELAWalletNew(transactions.getRawPointerArray().data(), transactions.size(),
								   WalletUnusedAddrs, WalletAllAddrs, setApplyFreeTx, WalletUpdateBalance,
								   WalletContainsTx, WalletAddUsedAddrs, WalletCreateTxForOutputs,
								   WalletMaxOutputAmount, WalletFeeForTx, TransactionIsSigned, KeyToAddress,
								   BalanceAfterTx);
			_subAccount->InitWallet(transactions.getRawPointerArray().data(), transactions.size(), _wallet);

			assert(listener != nullptr);
			_listener = boost::weak_ptr<Listener>(listener);

			Log::getLogger()->info("_wallet = {:p}, listener = {:p}", (void *) _wallet, (void *) listener.get());
			ParamChecker::checkCondition(_wallet == nullptr, Error::Wallet, "Create new wallet");

			BRWalletSetCallbacks((BRWallet *) _wallet, &_listener,
								 balanceChanged,
								 txAdded,
								 txUpdated,
								 txDeleted);

			typedef SharedWrapperList<Transaction, BRTransaction *> Transactions;
			for (Transactions::const_iterator it = transactions.cbegin(); it != transactions.cend(); ++it) {
				(*it)->isRegistered() = true;
			}

			ELAWalletLoadRemarks(_wallet, transactions);
		}

		Wallet::~Wallet() {
			if (_wallet != nullptr) {
				ELAWalletFree(_wallet);
				_wallet = nullptr;
			}
		}

		void Wallet::initListeningAddresses(const std::vector<std::string> &addrs) {
			_wallet->ListeningAddrs = addrs;
		}

		std::string Wallet::toString() const {
			//todo complete me
			return "";
		}

		BRWallet *Wallet::getRaw() const {
			return (BRWallet *) _wallet;
		}

		void Wallet::RegisterRemark(const TransactionPtr &transaction) {
			ELAWalletRegisterRemark(_wallet,
									Utils::UInt256ToString(transaction->getHash(), true),
									((ELATransaction *) transaction->getRaw())->Remark);
		}

		std::string Wallet::GetRemark(const std::string &txHash) {
			return ELAWalletGetRemark(_wallet, txHash);
		}

		nlohmann::json Wallet::GetBalanceInfo() {

			size_t utxosCount = BRWalletUTXOs((BRWallet *) _wallet, nullptr, 0);
			BRUTXO utxos[utxosCount];
			BRWalletUTXOs((BRWallet *) _wallet, utxos, utxosCount);

			nlohmann::json j;

			ELATransaction *t;
			std::map<std::string, uint64_t> addressesBalanceMap;
			pthread_mutex_lock(&_wallet->Raw.lock);
			for (size_t i = 0; i < utxosCount; ++i) {
				void *tempPtr = BRSetGet(_wallet->Raw.allTx, &utxos[utxosCount].hash);
				if (tempPtr == nullptr) continue;
				t = static_cast<ELATransaction *>(tempPtr);

				if (addressesBalanceMap.find(t->outputs[utxos->n]->getAddress()) != addressesBalanceMap.end()) {
					addressesBalanceMap[t->outputs[utxos->n]->getAddress()] += t->outputs[utxos->n]->getAmount();
				} else {
					addressesBalanceMap[t->outputs[utxos->n]->getAddress()] = t->outputs[utxos->n]->getAmount();
				}
			}
			pthread_mutex_unlock(&_wallet->Raw.lock);

			std::vector<nlohmann::json> balances;
			std::for_each(addressesBalanceMap.begin(), addressesBalanceMap.end(),
						  [&addressesBalanceMap, &balances](const std::map<std::string, uint64_t>::value_type &item) {
							  nlohmann::json balanceKeyValue;
							  balanceKeyValue[item.first] = item.second;
							  balances.push_back(balanceKeyValue);
						  });

			j["Balances"] = balances;
			return j;
		}

		uint64_t Wallet::GetBalanceWithAddress(const std::string &address) {
			size_t utxosCount = BRWalletUTXOs((BRWallet *) _wallet, nullptr, 0);
			BRUTXO utxos[utxosCount];
			BRWalletUTXOs((BRWallet *) _wallet, utxos, utxosCount);

			ELATransaction *t;
			uint64_t balance = 0;
			pthread_mutex_lock(&_wallet->Raw.lock);
			for (size_t i = 0; i < utxosCount; ++i) {
				void *tempPtr = BRSetGet(_wallet->Raw.allTx, &utxos[i].hash);
				if (tempPtr == nullptr) continue;
				t = static_cast<ELATransaction *>(tempPtr);
				if (BRAddressEq(t->outputs[utxos[i].n]->getRaw()->address, address.c_str())) {
					balance += t->outputs[utxos[i].n]->getAmount();
				}
			}
			pthread_mutex_unlock(&_wallet->Raw.lock);

			return balance;
		}

		SharedWrapperList<Transaction, BRTransaction *> Wallet::getTransactions() const {

			size_t transactionCount = BRWalletTransactions((BRWallet *) _wallet, NULL, 0);

			BRTransaction **transactions = (BRTransaction **) calloc(transactionCount, sizeof(BRTransaction *));
			transactionCount = BRWalletTransactions((BRWallet *) _wallet, transactions, transactionCount);

			SharedWrapperList<Transaction, BRTransaction *> results(transactionCount);
			// TODO: Decide if copy is okay; if not, be sure to mark 'isRegistered = true'
			//   We should not copy; but we need to deal with wallet-initiated 'free'
			for (int index = 0; index < transactionCount; index++) {
				results.push_back(TransactionPtr(new Transaction(*(ELATransaction *) transactions[index])));
			}

			if (NULL != transactions) free(transactions);
			return results;
		}

		SharedWrapperList<Transaction, BRTransaction *>
		Wallet::getTransactionsConfirmedBefore(uint32_t blockHeight) const {

			size_t transactionCount = BRWalletTxUnconfirmedBefore((BRWallet *) _wallet, NULL, 0, blockHeight);

			BRTransaction **transactions = (BRTransaction **) calloc(transactionCount, sizeof(BRTransaction *));
			transactionCount = BRWalletTxUnconfirmedBefore((BRWallet *) _wallet, transactions, transactionCount,
														   blockHeight);

			SharedWrapperList<Transaction, BRTransaction *> results(transactionCount);
			for (int index = 0; index < transactionCount; index++) {
				results.push_back(TransactionPtr(new Transaction(*(ELATransaction *) transactions[index])));
			}

			if (NULL != transactions) free(transactions);
			return results;
		}

		uint64_t Wallet::getBalance() const {
			return BRWalletBalance((BRWallet *) _wallet);
		}

		uint64_t Wallet::getTotalSent() {
			return BRWalletTotalSent((BRWallet *) _wallet);
		}

		uint64_t Wallet::getTotalReceived() {
			return BRWalletTotalReceived((BRWallet *) _wallet);
		}

		uint64_t Wallet::getFeePerKb() {
			return BRWalletFeePerKb((BRWallet *) _wallet);
		}

		void Wallet::setFeePerKb(uint64_t feePerKb) {
			BRWalletSetFeePerKb((BRWallet *) _wallet, feePerKb);
		}

		uint64_t Wallet::getMaxFeePerKb() {
			return MAX_FEE_PER_KB;
		}

		uint64_t Wallet::getDefaultFeePerKb() {
			return DEFAULT_FEE_PER_KB;
		}

		bool Wallet::AddressFilter(const std::string &fromAddress, const std::string &filterAddress) {
			return filterAddress == fromAddress;
		}

		void Wallet::SortUTXOForAmount(BRWallet *wallet, uint64_t amount) {
			if (array_count(wallet->utxos) <= 1)
				return;

			UTXO_t *utxos;
			BRUTXO *o;
			ELATransaction *tx;
			array_new(utxos, array_count(wallet->utxos));

			for (size_t i = 0; i < array_count(wallet->utxos); ++i) {
				o = &wallet->utxos[i];
				tx = (ELATransaction *) BRSetGet(wallet->allTx, o);
				if (!tx || o->n >= tx->outputs.size()) {
					Log::getLogger()->error("Invalid utxo {} n={}", Utils::UInt256ToString(o->hash, true), o->n);
					continue;
				}

				array_add(utxos, ((UTXO_t){*o, tx->outputs[o->n]->getAmount()}));
			}

			qsort(utxos, array_count(utxos), sizeof(*utxos), UTXOCompareAscending);

			uint64_t Threshold = amount * 2 + wallet->feePerKb;
			size_t ThresholdIndex = 0;
			for (size_t i = 0; i < array_count(utxos); ++i) {
				if (utxos[i].amount > Threshold) {
					ThresholdIndex = i;
					break;
				}
			}

			if (ThresholdIndex > 0)
				qsort(utxos, ThresholdIndex, sizeof(*utxos), UTXOCompareDescending);

			array_set_count(wallet->utxos, 0);

			for (size_t i = 0; i < array_count(utxos); ++i) {
				array_add(wallet->utxos, utxos[i].o);
			}

			array_free(utxos);
		}

		BRTransaction *Wallet::CreateTxForOutputs(BRWallet *wallet, const BRTxOutput outputs[], size_t outCount,
												  uint64_t fee, const std::string &fromAddress,
												  bool(*filter)(const std::string &fromAddress,
																const std::string &addr)) {
			ELATransaction *tx, *transaction = ELATransactionNew();
			uint64_t feeAmount, amount = 0, balance = 0;
			size_t i, cpfpSize = 0;
			BRUTXO *o;
			BRAddress addr = BR_ADDRESS_NONE;
			TransactionPtr txn(new Transaction(transaction, false));

			assert(wallet != NULL);
			ParamChecker::checkCondition(outputs == NULL || outCount == 0, Error::CreateTransaction, "Invalid outputs");

			for (i = 0; outputs && i < outCount; i++) {
				ELATxOutput *ELAOutput = (ELATxOutput *)&outputs[i];

				transaction->outputs.push_back(new TransactionOutput(ELAOutput->raw.address, ELAOutput->raw.amount,
																	 ELAOutput->assetId));
				amount += outputs[i].amount;
			}

			pthread_mutex_lock(&wallet->lock);
			feeAmount = txn->calculateFee(wallet->feePerKb);

			SortUTXOForAmount(wallet, amount);
			// TODO: use up all UTXOs for all used addresses to avoid leaving funds in addresses whose public key is revealed
			// TODO: avoid combining addresses in a single transaction when possible to reduce information leakage
			// TODO: use up UTXOs received from any of the output scripts that this transaction sends funds to, to mitigate an
			//       attacker double spending and requesting a refund
			for (i = 0; i < array_count(wallet->utxos); i++) {
				o = &wallet->utxos[i];
				tx = (ELATransaction *) BRSetGet(wallet->allTx, o);
				if (!tx || o->n >= tx->outputs.size()) continue;
				if (filter && !fromAddress.empty() && !filter(fromAddress, tx->outputs[o->n]->getAddress())) {
					continue;
				}

				if (tx->raw.blockHeight >= wallet->lastBlockHeight) {
					Log::getLogger()->warn("utxo: '{}' n: '{}', confirming, can't spend for now, tx height = {}, wallet block height = {}",
										   Utils::UInt256ToString(tx->raw.txHash, true), o->n, tx->raw.blockHeight, wallet->lastBlockHeight);
					continue;
				}

				BRTransactionAddInput(&transaction->raw, tx->raw.txHash, o->n, tx->outputs[o->n]->getAmount(),
									  tx->outputs[o->n]->getRaw()->script, tx->outputs[o->n]->getRaw()->scriptLen,
									  nullptr, 0, TXIN_SEQUENCE);
				std::string addr = Utils::UInt168ToAddress(tx->outputs[o->n]->getProgramHash());
				size_t inCount = transaction->raw.inCount;
				BRTxInput *input = &transaction->raw.inputs[inCount - 1];
				memset(input->address, 0, sizeof(input->address));
				strncpy(input->address, addr.c_str(), sizeof(input->address) - 1);

				if (txn->getSize() + TX_RECHARGE_OUTPUT_SIZE > TX_MAX_SIZE) { // transaction size-in-bytes too large
					bool balanceEnough = true;
					feeAmount = txn->calculateFee(wallet->feePerKb) + wallet->feePerKb;
					ELATransactionFree(transaction);
					transaction = nullptr;

					// check for sufficient total funds before building a smaller transaction
					if (wallet->balance < amount + feeAmount) {
						Log::getLogger()->error("Not enough sufficient total funds for building a smaller tx.");
						balanceEnough = false;
					}

					pthread_mutex_unlock(&wallet->lock);

					ParamChecker::checkCondition(!balanceEnough, Error::CreateTransaction,
												 "Available token is not enough");

					uint64_t maxAmount = 0;
					if (outputs[outCount - 1].amount > (amount + feeAmount - balance)) {
						for (int j = 0; j < outCount - 1; ++j) {
							maxAmount += outputs[j].amount;
						}
						maxAmount += outputs[outCount - 1].amount - (amount + feeAmount - balance);
						ParamChecker::checkCondition(true, Error::CreateTransactionExceedSize,
													 "Tx size too large, amount should less than " +
													 std::to_string(maxAmount), maxAmount);
					} else {
						for (int j = 0; j < outCount - 1; ++j) {
							maxAmount += outputs[j].amount;
						}
						ParamChecker::checkCondition(true, Error::CreateTransactionExceedSize,
													 "Tx size too large, amount should less than " +
													 std::to_string(maxAmount), maxAmount);
					}

					balance = amount = feeAmount = 0;
					pthread_mutex_lock(&wallet->lock);
					break;
				}

				balance += tx->outputs[o->n]->getAmount();

//        // size of unconfirmed, non-change inputs for child-pays-for-parent fee
//        // don't include parent tx with more than 10 inputs or 10 outputs
//        if (tx->blockHeight == TX_UNCONFIRMED && tx->inCount <= 10 && tx->outCount <= 10 &&
//            ! _BRWalletTxIsSend(wallet, tx)) cpfpSize += BRTransactionSize(tx);

				// fee amount after adding a change output
				feeAmount = txn->calculateFee(wallet->feePerKb);

				// increase fee to round off remaining wallet balance to nearest 100 satoshi
				//if (wallet->balance > amount + feeAmount) feeAmount += (wallet->balance - (amount + feeAmount)) % 100;

				if (balance >= amount + feeAmount) break;
			}

			pthread_mutex_unlock(&wallet->lock);

			if (transaction != nullptr) {
				transaction->fee = feeAmount;
			}

			if (transaction && (outCount < 1 || balance < amount + feeAmount)) { // no outputs/insufficient funds
				ELATransactionFree(transaction);
				transaction = nullptr;
				ParamChecker::checkCondition(balance < amount + feeAmount, Error::BalanceNotEnough,
											 "Available token is not enough");
				ParamChecker::checkCondition(outCount < 1, Error::CreateTransaction, "Output count is not enough");
			} else if (transaction && balance - (amount + feeAmount) > 0) { // add change output
				wallet->WalletUnusedAddrs(wallet, &addr, 1, 1);

				UInt256 assetID = transaction->outputs[0]->getAssetId();

				TransactionOutput *output = new TransactionOutput(addr.s, balance - (amount + feeAmount), assetID);

				transaction->outputs.push_back(output);
			}

			return (BRTransaction *) transaction;
		}

		BRTransaction *Wallet::WalletCreateTxForOutputs(BRWallet *wallet, const BRTxOutput outputs[], size_t outCount) {
			return CreateTxForOutputs(wallet, outputs, outCount, 0, "", nullptr);
		}

		TransactionPtr
		Wallet::createTransaction(const std::string &fromAddress, uint64_t fee, uint64_t amount,
								  const std::string &toAddress, const std::string &remark,
								  const std::string &memo) {
			UInt168 u168Address = UINT168_ZERO;
			ParamChecker::checkCondition(!fromAddress.empty() && !Utils::UInt168FromAddress(u168Address, fromAddress),
										 Error::CreateTransaction, "Invalid spender address " + fromAddress);

			ParamChecker::checkCondition(!Utils::UInt168FromAddress(u168Address, toAddress), Error::CreateTransaction,
										 "Invalid receiver address " + toAddress);

			TransactionOutputPtr output = TransactionOutputPtr(new TransactionOutput(
				toAddress, amount, Key::getSystemAssetId()));

			BRTxOutput outputs[1];
			outputs[0] = *output->getRaw();

			ELATransaction *tx = (ELATransaction *) CreateTxForOutputs((BRWallet *) _wallet, outputs, 1, fee,
																	   fromAddress, AddressFilter);

			TransactionPtr result = nullptr;
			if (tx != nullptr) {
				result = TransactionPtr(new Transaction(tx));
				result->setRemark(remark);

				result->addAttribute(
					new Attribute(Attribute::Nonce, Utils::convertToMemBlock(std::to_string(std::rand()))));
				if (!memo.empty())
					result->addAttribute(new Attribute(Attribute::Memo, Utils::convertToMemBlock(memo)));
				if (tx->type == ELATransaction::TransferCrossChainAsset)
					result->addAttribute(
						new Attribute(Attribute::Confirmations, Utils::convertToMemBlock(std::to_string(1))));
			}

			return result;
		}

		bool Wallet::containsTransaction(const TransactionPtr &transaction) {
			return BRWalletContainsTransaction((BRWallet *) _wallet, transaction->getRaw()) != 0;
		}

		bool Wallet::inputFromWallet(const BRTxInput *in) {
			BRWallet *wallet = &_wallet->Raw;
			for (size_t i = 0; i < array_count(wallet->transactions); i++) {
				ELATransaction *tx = (ELATransaction *) wallet->transactions[i];
				if (UInt256Eq(&in->txHash, &tx->raw.txHash)) {
					if (containsAddress(tx->outputs[in->index]->getAddress())) {
						return true;
					}
				}
			}

			return false;
		}

		bool Wallet::registerTransaction(const TransactionPtr &transaction) {
			return BRWalletRegisterTransaction((BRWallet *) _wallet, transaction->getRaw()) != 0;
		}

		void Wallet::removeTransaction(const UInt256 &transactionHash) {
			BRWalletRemoveTransaction((BRWallet *) _wallet, transactionHash);
		}

		void Wallet::updateTransactions(
			const std::vector<UInt256> &transactionsHashes, uint32_t blockHeight, uint32_t timestamp) {
			BRWalletUpdateTransactions((BRWallet *) _wallet, transactionsHashes.data(),
									   transactionsHashes.size(), blockHeight, timestamp);
		}

		TransactionPtr Wallet::transactionForHash(const UInt256 &transactionHash) {

			BRTransaction *transaction = BRWalletTransactionForHash((BRWallet *) _wallet, transactionHash);
			return TransactionPtr(new Transaction(*(ELATransaction *) transaction));
		}

		bool Wallet::transactionIsValid(const TransactionPtr &transaction) {
			return BRWalletTransactionIsValid((BRWallet *) _wallet, transaction->getRaw()) != 0;
		}

		bool Wallet::transactionIsPending(const TransactionPtr &transaction) {
			return BRWalletTransactionIsPending((BRWallet *) _wallet, transaction->getRaw()) != 0;
		}

		bool Wallet::transactionIsVerified(const TransactionPtr &transaction) {
			return BRWalletTransactionIsVerified((BRWallet *) _wallet, transaction->getRaw()) != 0;
		}

		uint64_t Wallet::getTransactionAmount(const TransactionPtr &tx) {
			uint64_t amountSent = getTransactionAmountSent(tx);
			uint64_t amountReceived = getTransactionAmountReceived(tx);

			return amountSent == 0
				   ? amountReceived
				   : -1 * (amountSent - amountReceived + getTransactionFee(tx));
		}

		uint64_t Wallet::getTransactionFee(const TransactionPtr &tx) {
			return WalletFeeForTx((BRWallet *) _wallet, tx->getRaw());
		}

		uint64_t Wallet::getTransactionAmountSent(const TransactionPtr &tx) {

			return BRWalletAmountSentByTx((BRWallet *) _wallet, tx->getRaw());
		}

		uint64_t Wallet::getTransactionAmountReceived(const TransactionPtr &tx) {

			return BRWalletAmountReceivedFromTx((BRWallet *) _wallet, tx->getRaw());
		}

		uint64_t Wallet::getBalanceAfterTransaction(const TransactionPtr &transaction) {

			return _wallet->Raw.balanceAfterTx((BRWallet *) _wallet, transaction->getRaw());
		}

		std::string Wallet::getTransactionAddress(const TransactionPtr &transaction) {

			return getTransactionAmount(transaction) > 0
				   ? getTransactionAddressInputs(transaction)   // we received -> from inputs
				   : getTransactionAddressOutputs(transaction); // we sent     -> to outputs
		}

		std::string Wallet::getTransactionAddressInputs(const TransactionPtr &transaction) {

			for (size_t i = 0; i < transaction->getRaw()->inCount; i++) {

				std::string address = transaction->getRaw()->inputs[i].address;
				if (!containsAddress(address))
					return address;
			}
			return "";
		}

		std::string Wallet::getTransactionAddressOutputs(const TransactionPtr &transaction) {

			const std::vector<TransactionOutput *> &outputs = transaction->getOutputs();
			for (size_t i = 0; i < outputs.size(); i++) {

				std::string address = outputs[i]->getAddress();
				if (!containsAddress(address))
					return address;
			}
			return "";
		}

		uint64_t Wallet::getFeeForTransactionSize(size_t size) {
			return BRWalletFeeForTxSize((BRWallet *) _wallet, size);
		}

		uint64_t Wallet::getMinOutputAmount() {

			return BRWalletMinOutputAmount((BRWallet *) _wallet);
		}

		uint64_t Wallet::getMaxOutputAmount() {

			return WalletMaxOutputAmount((BRWallet *) _wallet);
		}

		std::string Wallet::getReceiveAddress() const {

			return BRWalletReceiveAddress((BRWallet *) _wallet).s;
		}

		std::vector<std::string> Wallet::getAllAddresses() {

			size_t addrCount = _wallet->Raw.WalletAllAddrs((BRWallet *) _wallet, NULL, 0);

			BRAddress addresses[addrCount];
			_wallet->Raw.WalletAllAddrs((BRWallet *) _wallet, addresses, addrCount);

			std::vector<std::string> results;
			for (int i = 0; i < addrCount; i++) {
				results.push_back(addresses[i].s);
			}
			return results;
		}

		bool Wallet::containsAddress(const std::string &address) {
			if (_wallet->IsSingleAddress) {
				return _wallet->SingleAddress == address;
			}

			return BRWalletContainsAddress((BRWallet *) _wallet, address.c_str()) != 0;
		}

		bool Wallet::addressIsUsed(const std::string &address) {

			return BRWalletAddressIsUsed((BRWallet *) _wallet, address.c_str()) != 0;
		}

		// maximum amount that can be sent from the wallet to a single address after fees
		uint64_t Wallet::WalletMaxOutputAmount(BRWallet *wallet) {
			ELATransaction *tx;
			BRUTXO *o;
			uint64_t fee, amount = 0;
			size_t i, txSize, cpfpSize = 0, inCount = 0;

			assert(wallet != NULL);
			pthread_mutex_lock(&wallet->lock);

			for (i = array_count(wallet->utxos); i > 0; i--) {
				o = &wallet->utxos[i - 1];
				tx = (ELATransaction *) BRSetGet(wallet->allTx, &o->hash);
				if (!tx || o->n >= tx->outputs.size()) continue;
				inCount++;
				amount += tx->outputs[o->n]->getAmount();

//        // size of unconfirmed, non-change inputs for child-pays-for-parent fee
//        // don't include parent tx with more than 10 inputs or 10 outputs
//        if (tx->blockHeight == TX_UNCONFIRMED && tx->inCount <= 10 && tx->outCount <= 10 &&
//            ! _BRWalletTxIsSend(wallet, tx)) cpfpSize += BRTransactionSize(tx);
			}

			txSize = 8 + BRVarIntSize(inCount) + TX_INPUT_SIZE * inCount + BRVarIntSize(2) + TX_OUTPUT_SIZE * 2;
			fee = _txFee(wallet->feePerKb, txSize + cpfpSize);
			pthread_mutex_unlock(&wallet->lock);

			return (amount > fee) ? amount - fee : 0;
		}

		uint64_t Wallet::WalletFeeForTx(BRWallet *wallet, const BRTransaction *tx) {
			uint64_t amount = 0;

			assert(tx != nullptr);
			if (tx == nullptr) {
				return amount;
			}

			ELATransaction *txn = (ELATransaction *) tx;
			pthread_mutex_lock(&wallet->lock);

			for (size_t i = 0; txn && i < txn->raw.inCount && amount != UINT64_MAX; i++) {
				ELATransaction *t = (ELATransaction *) BRSetGet(wallet->allTx, &txn->raw.inputs[i].txHash);
				uint32_t n = txn->raw.inputs[i].index;

				if (t && n < t->outputs.size()) {
					amount += t->outputs[n]->getAmount();
				} else amount = UINT64_MAX;
			}

			pthread_mutex_unlock(&wallet->lock);

			for (size_t i = 0; txn && i < txn->outputs.size() && amount != UINT64_MAX; i++) {
				amount -= txn->outputs[i]->getAmount();
			}

			return amount;
		}

		void Wallet::WalletUpdateBalance(BRWallet *wallet) {
			int isInvalid, isPending;
			uint64_t balance = 0, prevBalance = 0;
			time_t now = time(NULL);
			size_t i, j;
			ELATransaction *tx, *t;

			array_clear(wallet->utxos);
			array_clear(wallet->balanceHist);
			BRSetClear(wallet->spentOutputs);
			BRSetClear(wallet->invalidTx);
			BRSetClear(wallet->pendingTx);
			BRSetClear(wallet->usedAddrs);
			wallet->totalSent = 0;
			wallet->totalReceived = 0;

			for (i = 0; i < array_count(wallet->transactions); i++) {
				tx = (ELATransaction *) wallet->transactions[i];

				// check if any inputs are invalid or already spent
				if (tx->raw.blockHeight == TX_UNCONFIRMED) {
					for (j = 0, isInvalid = 0; !isInvalid && j < tx->raw.inCount; j++) {
						if (BRSetContains(wallet->spentOutputs, &tx->raw.inputs[j]) ||
							BRSetContains(wallet->invalidTx, &tx->raw.inputs[j].txHash))
							isInvalid = 1;
					}

					if (isInvalid) {
						BRSetAdd(wallet->invalidTx, tx);
						array_add(wallet->balanceHist, balance);
						continue;
					}
				}

				// add inputs to spent output set
				for (j = 0; j < tx->raw.inCount; j++) {
					BRSetAdd(wallet->spentOutputs, &tx->raw.inputs[j]);
				}

				// check if tx is pending
				if (tx->raw.blockHeight == TX_UNCONFIRMED) {
					isPending = (ELATransactionSize(tx) > TX_MAX_SIZE) ? 1 : 0; // check tx size is under TX_MAX_SIZE

					for (j = 0; !isPending && j < tx->outputs.size(); j++) {
						if (tx->outputs[j]->getAmount() < TX_MIN_OUTPUT_AMOUNT)
							isPending = 1; // check that no outputs are dust
					}

					for (j = 0; !isPending && j < tx->raw.inCount; j++) {
						if (tx->raw.inputs[j].sequence < UINT32_MAX - 1) isPending = 1; // check for replace-by-fee
						if (tx->raw.inputs[j].sequence < UINT32_MAX && tx->raw.lockTime < TX_MAX_LOCK_HEIGHT &&
							tx->raw.lockTime > wallet->blockHeight + 1)
							isPending = 1; // future lockTime
						if (tx->raw.inputs[j].sequence < UINT32_MAX && tx->raw.lockTime > now)
							isPending = 1; // future lockTime
						if (BRSetContains(wallet->pendingTx, &tx->raw.inputs[j].txHash))
							isPending = 1; // check for pending inputs
						// TODO: XXX handle BIP68 check lock time verify rules
					}

					if (isPending) {
						BRSetAdd(wallet->pendingTx, tx);
						array_add(wallet->balanceHist, balance);
						continue;
					}
				}

				// add outputs to UTXO set
				// TODO: don't add outputs below TX_MIN_OUTPUT_AMOUNT
				// TODO: don't add coin generation outputs < 100 blocks deep
				// NOTE: balance/UTXOs will then need to be recalculated when last block changes
				for (j = 0; tx->raw.blockHeight != TX_UNCONFIRMED && j < tx->outputs.size(); j++) {
					if (tx->outputs[j]->getRaw()->address[0] != '\0') {
						if (((ELAWallet *) wallet)->IsSingleAddress) {
							ELAWallet *elaWallet = (ELAWallet *) wallet;
							if (elaWallet->SingleAddress == std::string(tx->outputs[j]->getRaw()->address)) {
								array_add(wallet->utxos, ((BRUTXO) {tx->raw.txHash, (uint32_t) j}));
								balance += tx->outputs[j]->getAmount();
							}
						} else {
							BRSetAdd(wallet->usedAddrs, tx->outputs[j]->getRaw()->address);

							if (BRSetContains(wallet->allAddrs, tx->outputs[j]->getRaw()->address)) {
								array_add(wallet->utxos, ((BRUTXO) {tx->raw.txHash, (uint32_t) j}));
								balance += tx->outputs[j]->getAmount();
							}
						}
					}
				}

				// transaction ordering is not guaranteed, so check the entire UTXO set against the entire spent output set
				for (j = array_count(wallet->utxos); j > 0; j--) {
					if (!BRSetContains(wallet->spentOutputs, &wallet->utxos[j - 1])) continue;
					t = (ELATransaction *) BRSetGet(wallet->allTx, &wallet->utxos[j - 1].hash);
					balance -= t->outputs[wallet->utxos[j - 1].n]->getAmount();
					array_rm(wallet->utxos, j - 1);
				}

				if (prevBalance < balance) wallet->totalReceived += balance - prevBalance;
				if (balance < prevBalance) wallet->totalSent += prevBalance - balance;
				array_add(wallet->balanceHist, balance);
				prevBalance = balance;
			}

			assert(array_count(wallet->balanceHist) == array_count(wallet->transactions));
			wallet->balance = balance;
		}

		int Wallet::WalletContainsTx(BRWallet *wallet, const BRTransaction *tx) {
			int r = 0;

			ELAWallet *elaWallet = (ELAWallet *) wallet;
			const ELATransaction *txn = (const ELATransaction *) tx;

			if (!txn)
				return r;

			size_t outCount = txn->outputs.size();

			for (size_t i = 0; !r && i < outCount; i++) {
				if (elaWallet->IsSingleAddress) {
					if (elaWallet->SingleAddress == std::string(txn->outputs[i]->getRaw()->address)) {
						r = 1;
					}
				} else {
					if (BRSetContains(wallet->allAddrs, txn->outputs[i]->getRaw()->address)) {
						r = 1;
					}
				}
			}

			for (size_t i = 0; !r && i < txn->raw.inCount; i++) {
				ELATransaction *t = (ELATransaction *) BRSetGet(wallet->allTx, &txn->raw.inputs[i].txHash);
				uint32_t n = txn->raw.inputs[i].index;

				if (t == nullptr || n >= t->outputs.size()) {
					continue;
				}

				if (elaWallet->IsSingleAddress) {
					if (elaWallet->SingleAddress == std::string(t->outputs[n]->getRaw()->address)) {
						r = 1;
					}
				} else {
					if (BRSetContains(wallet->allAddrs, t->outputs[n]->getRaw()->address)) {
						r = 1;
					}
				}
			}

			//for listening addresses
			for (size_t i = 0; i < outCount; ++i) {
				if (std::find(elaWallet->ListeningAddrs.begin(), elaWallet->ListeningAddrs.end(),
							  txn->outputs[i]->getRaw()->address) != elaWallet->ListeningAddrs.end())
					r = 1;
			}

			return r;
		}

		void Wallet::WalletAddUsedAddrs(BRWallet *wallet, const BRTransaction *tx) {
			const ELATransaction *txn = (const ELATransaction *) tx;

			if (!txn)
				return;

			size_t outCount = txn->outputs.size();
			for (size_t j = 0; j < outCount; j++) {
				if (txn->outputs[j]->getRaw()->address[0] != '\0')
					BRSetAdd(wallet->usedAddrs, txn->outputs[j]->getRaw()->address);
			}
		}

		int Wallet::TransactionIsSigned(const BRTransaction *tx) {
			return true == ELATransactionIsSign((ELATransaction *) tx);
		}

		void Wallet::setApplyFreeTx(void *info, void *tx) {
			ELATransactionFree((ELATransaction *) tx);
		}

		typedef boost::weak_ptr<Wallet::Listener> WeakListener;

		void Wallet::balanceChanged(void *info, uint64_t balance) {

			WeakListener *listener = (WeakListener *) info;
			if (!listener->expired()) {
				listener->lock()->balanceChanged(balance);
			}
		}

		void Wallet::txAdded(void *info, BRTransaction *tx) {

			WeakListener *listener = (WeakListener *) info;
			if (!listener->expired()) {
				listener->lock()->onTxAdded(TransactionPtr(new Transaction((ELATransaction *) tx, false)));
			}
		}

		void Wallet::txUpdated(void *info, const UInt256 txHashes[], size_t count, uint32_t blockHeight,
							   uint32_t timestamp) {

			WeakListener *listener = (WeakListener *) info;
			if (!listener->expired()) {

				// Invoke the callback for each of txHashes.
				for (size_t i = 0; i < count; i++) {
					listener->lock()->onTxUpdated(Utils::UInt256ToString(txHashes[i], true), blockHeight, timestamp);
				}
			}
		}

		void Wallet::txDeleted(void *info, UInt256 txHash, int notifyUser, int recommendRescan) {

			WeakListener *listener = (WeakListener *) info;
			if (!listener->expired()) {
				listener->lock()->onTxDeleted(Utils::UInt256ToString(txHash, true), static_cast<bool>(notifyUser),
											  static_cast<bool>(recommendRescan));
			}
		}

		size_t Wallet::KeyToAddress(const BRKey *key, char *addr, size_t addrLen) {
			BRKey *brKey = new BRKey;
			memcpy(brKey, key, sizeof(BRKey));

			KeyPtr keyPtr(new Key(brKey));

			std::string address = keyPtr->address();

			memset(addr, '\0', addrLen);
			strncpy(addr, address.c_str(), addrLen - 1);

			return address.size();
		}

		uint32_t Wallet::getBlockHeight() const {
			return _wallet->Raw.blockHeight;
		}

		size_t Wallet::WalletUnusedAddrs(BRWallet *wallet, BRAddress addrs[], uint32_t gapLimit, int internal) {
			ELAWallet *elaWallet = (ELAWallet *) wallet;
			if (elaWallet->IsSingleAddress) {
				if (addrs != NULL) {
					memset(addrs[0].s, 0, sizeof(addrs[0].s));
					strncpy(addrs[0].s, elaWallet->SingleAddress.c_str(), sizeof(addrs[0].s) - 1);
				}
				return 1;
			}

			BRAddress *addrChain, emptyAddress = BR_ADDRESS_NONE;
			size_t i, j = 0, count, startCount;
			uint32_t chain = (internal) ? SEQUENCE_INTERNAL_CHAIN : SEQUENCE_EXTERNAL_CHAIN;

			assert(wallet != NULL);
			assert(gapLimit > 0);
			pthread_mutex_lock(&wallet->lock);
			addrChain = (internal) ? wallet->internalChain : wallet->externalChain;
			i = count = startCount = array_count(addrChain);

			// keep only the trailing contiguous block of addresses with no transactions
			while (i > 0 && !BRSetContains(wallet->usedAddrs, &addrChain[i - 1])) i--;

			while (i + gapLimit > count) { // generate new addresses up to gapLimit
				Key key;
				BRAddress address = BR_ADDRESS_NONE;

				size_t len = BRBIP32PubKey(NULL, 0, wallet->masterPubKey, chain, count);
				CMBlock pubKey(len);
				BRBIP32PubKey(pubKey, pubKey.GetSize(), wallet->masterPubKey, chain, count);

				if (!key.setPubKey(pubKey)) break;
				if (!wallet->KeyToAddress(key.getRaw(), address.s, sizeof(BRAddress)) ||
					BRAddressEq(&address, &emptyAddress))
					break;

				array_add(addrChain, address);
				count++;
				if (BRSetContains(wallet->usedAddrs, &address)) i = count;
			}

			if (addrs && i + gapLimit <= count) {
				for (j = 0; j < gapLimit; j++) {
					addrs[j] = addrChain[i + j];
				}
			}

			// was addrChain moved to a new memory location?
			if (addrChain == (internal ? wallet->internalChain : wallet->externalChain)) {
				for (i = startCount; i < count; i++) {
					BRSetAdd(wallet->allAddrs, &addrChain[i]);
				}
			} else {
				if (internal) wallet->internalChain = addrChain;
				if (!internal) wallet->externalChain = addrChain;
				BRSetClear(wallet->allAddrs); // clear and rebuild allAddrs

				for (i = array_count(wallet->internalChain); i > 0; i--) {
					BRSetAdd(wallet->allAddrs, &wallet->internalChain[i - 1]);
				}

				for (i = array_count(wallet->externalChain); i > 0; i--) {
					BRSetAdd(wallet->allAddrs, &wallet->externalChain[i - 1]);
				}
			}

			pthread_mutex_unlock(&wallet->lock);
			return j;
		}

		uint64_t Wallet::BalanceAfterTx(BRWallet *wallet, const BRTransaction *tx) {
			uint64_t balance;

			assert(wallet != NULL);
			assert(tx != NULL && wallet->TransactionIsSigned(tx));
			pthread_mutex_lock(&wallet->lock);
			balance = wallet->balance;

			for (size_t i = array_count(wallet->transactions); tx && i > 0; i--) {
				if (!BRTransactionEq(tx, wallet->transactions[i - 1])) continue;

				balance = wallet->balanceHist[i - 1];
				break;
			}

			pthread_mutex_unlock(&wallet->lock);
			return balance;
		}

		size_t Wallet::WalletAllAddrs(BRWallet *wallet, BRAddress *addrs, size_t addrsCount) {
			ELAWallet *elaWallet = (ELAWallet *) wallet;
			if (elaWallet->IsSingleAddress) {
				if (addrs != NULL && addrsCount > 0) {
					memset(addrs[0].s, 0, sizeof(addrs[0].s));
					strncpy(addrs[0].s, elaWallet->SingleAddress.c_str(), sizeof(addrs[0].s) - 1);
				}
				return 1;
			}
			return BRWalletAllAddrs(wallet, addrs, addrsCount);
		}

		void Wallet::signTransaction(const boost::shared_ptr<Transaction> &transaction, int forkId,
									 const std::string &payPassword) {
			ParamChecker::checkCondition(transaction.get() == nullptr, Error::InvalidArgument, "Sign null tx");
			_subAccount->SignTransaction(transaction, _wallet, payPassword);
		}

	}
}