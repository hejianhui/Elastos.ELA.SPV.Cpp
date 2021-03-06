// Copyright (c) 2012-2018 The Elastos Open Source Project
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <SDK/Wrapper/Wallet.h>
#include <SDK/Common/ParamChecker.h>
#include <SDK/Common/Utils.h>
#include "SingleSubAccount.h"

namespace Elastos {
	namespace ElaWallet {

		SingleSubAccount::SingleSubAccount(IAccount *account) :
				SubAccountBase(account) {

		}

		SingleSubAccount::~SingleSubAccount() {
		}

		void SingleSubAccount::InitWallet(BRTransaction *transactions[], size_t txCount, ELAWallet *wallet) {
			wallet->IsSingleAddress = true;
			wallet->SingleAddress = _parentAccount->GetAddress();

			wallet->Raw.WalletUpdateBalance((BRWallet *) wallet);
		}

		Key SingleSubAccount::DeriveMainAccountKey(const std::string &payPassword) {
			return _parentAccount->DeriveKey(payPassword);
		}

		std::string SingleSubAccount::GetMainAccountPublicKey() const {
			return _parentAccount->GetPublicKey();
		}

		WrapperList<Key, BRKey>
		SingleSubAccount::DeriveAccountAvailableKeys(const std::string &payPassword,
													 const Elastos::ElaWallet::TransactionPtr &transaction) {
			WrapperList<Key, BRKey> result;
			result.push_back(_parentAccount->DeriveKey(payPassword));
			return result;
		}

		void SingleSubAccount::SignTransaction(const TransactionPtr &transaction, ELAWallet *wallet,
											   const std::string &payPassword) {
			WrapperList<Key, BRKey> keyList = DeriveAccountAvailableKeys(payPassword, transaction);
			ParamChecker::checkCondition(!transaction->sign(keyList, 0), Error::Sign,
										 "Transaction Sign error!");
		}

		nlohmann::json SingleSubAccount::GetBasicInfo() const {
			nlohmann::json j;
			j["Type"] = "Single Account";
			return j;
		}
	}
}

