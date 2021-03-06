// Copyright (c) 2012-2018 The Elastos Open Source Project
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SPVSDK_SUBACCOUNTUTILS_H
#define SPVSDK_SUBACCOUNTUTILS_H

#include "ISubAccount.h"
#include "KeyStore/CoinInfo.h"

namespace Elastos {
	namespace ElaWallet {

		class SubAccountGenerator {
		public:
			SubAccountGenerator();

			~SubAccountGenerator();

			ISubAccount *Generate();

			void SetCoinInfo(const CoinInfo &coinInfo);

			void SetParentAccount(IAccount *account);

			void SetMasterPubKey(const MasterPubKeyPtr &masterPubKey);

			void Clean();

			const CMBlock &GetResultPublicKey() const;

			const UInt256 &GetResultChainCode() const;

			static MasterPubKeyPtr
			GenerateMasterPubKey(IAccount *account, uint32_t coinIndex, const std::string &payPassword);

		private:
			ISubAccount *GenerateFromCoinInfo(IAccount *account, const CoinInfo &coinInfo);

			ISubAccount *GenerateFromHDPath(IAccount *account, uint32_t coinIndex);

		private:
			CoinInfo _coinInfo;
			IAccount *_parentAccount;

			CMBlock _resultPubKey;
			UInt256 _resultChainCode;
			MasterPubKeyPtr _masterPubKey;
		};

	}
}

#endif //SPVSDK_SUBACCOUNTUTILS_H
