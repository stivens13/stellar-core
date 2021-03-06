// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <string>       // std::string
#include <iostream>     // std::cout
#include <sstream>   

namespace stellar
{

using namespace std;

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           OperationResult& res,
                                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

bool
CreateAccountOpFrame::doApply(Application& app, LedgerDelta& delta,
                              LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

    mDestAccount =
        AccountFrame::loadAccount(delta, mCreateAccount.destination, db);
    if (!mDestAccount)
    {
        
        /*
            Check for right autorization
            FOUNDATION 0
            VENDOR 1
            MERCHANT 2
            CLIENT 3
            FOUNDATION < VENDOR < MERCHANT < CLIENT 
            if source level is higher than new account created,
            then validation failed
        */

        if(!validateAccountTypes())
        {
            app.getMetrics()
                .NewMeter({"op-create-account", "failure", "underauthorized"},
                        "operation")
                .Mark();
            innerResult().code(CREATE_ACCOUNT_UNDERAUTHORIZED);

            return false;
        }

//         if (mCreateAccount.startingBalance < ledgerManager.getMinBalance(0))
        if (mCreateAccount.startingBalance < 10)
        { // not over the minBalance to make an account
            app.getMetrics()
                .NewMeter({"op-create-account", "failure", "low-reserve"},
                          "operation")
                .Mark();
            innerResult().code(CREATE_ACCOUNT_LOW_RESERVE);
            return false;
        }
        else
        {
            int64_t minBalance =
                mSourceAccount->getMinimumBalance(ledgerManager);

            if ((mSourceAccount->getAccount().balance - minBalance) <
                mCreateAccount.startingBalance)
            { // they don't have enough to send
                app.getMetrics()
                    .NewMeter({"op-create-account", "failure", "underfunded"},
                              "operation")
                    .Mark();
                innerResult().code(CREATE_ACCOUNT_UNDERFUNDED);
                return false;
            }

            auto ok =
                mSourceAccount->addBalance(-mCreateAccount.startingBalance);
            assert(ok);

            mSourceAccount->storeChange(delta, db);

            mDestAccount = make_shared<AccountFrame>(mCreateAccount.destination);
            mDestAccount->getAccount().seqNum =
                delta.getHeaderFrame().getStartingSequenceNumber();
            mDestAccount->getAccount().balance = mCreateAccount.startingBalance;
            mDestAccount->getAccount().accountType = mCreateAccount.accountType;

            mDestAccount->storeAdd(delta, db);

            app.getMetrics()
                .NewMeter({"op-create-account", "success", "apply"},
                          "operation")
                .Mark();
            innerResult().code(CREATE_ACCOUNT_SUCCESS);
            return true;
        }
    }
    else
    {
        app.getMetrics()
            .NewMeter({"op-create-account", "failure", "already-exist"},
                      "operation")
            .Mark();
        innerResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }
}

bool
CreateAccountOpFrame::validateAccountTypes()
{
    uint32 sourceType = mSourceAccount->getAccount().accountType;
    uint32 destType = mCreateAccount.accountType;
    if(     (sourceType == FOUNDATION && destType == LBO)
        ||  (sourceType == FOUNDATION && destType == OPERATOR)
        ||  (sourceType == FOUNDATION && destType == ISSUER)
        ||  (sourceType == LBO && destType == ISSUER)
        ||  (destType  == CLIENT) )
    {
        return true;
    }

    return false;

}

bool
CreateAccountOpFrame::doCheckValid(Application& app)
{
    if (mCreateAccount.startingBalance <= 0)
    {
        app.getMetrics()
            .NewMeter(
                {"op-create-account", "invalid", "malformed-negative-balance"},
                "operation")
            .Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    if (mCreateAccount.destination == getSourceID())
    {
        app.getMetrics()
            .NewMeter({"op-create-account", "invalid",
                       "malformed-destination-equals-source"},
                      "operation")
            .Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }
    
    return true;
}
}
