//
// Created by sanhuazhang on 2019/05/23
//

/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MigrateHandle.hpp"
#include "Assertion.hpp"
#include "CoreConst.h"
#include "Time.hpp"
#include <cmath>

namespace WCDB {

MigrateHandle::MigrateHandle()
: m_migratingInfo(nullptr)
, m_migrateStatement(getStatement())
, m_removeMigratedStatement(getStatement())
, m_samplePointing(0)
{
}

MigrateHandle::~MigrateHandle()
{
    finalizeMigrationStatement();
    returnStatement(m_migrateStatement);
    returnStatement(m_removeMigratedStatement);
}

bool MigrateHandle::reAttach(const MigrationBaseInfo* info)
{
    WCTAssert(!isInTransaction());
    WCTAssert(!isPrepared());

    bool succeed = true;
    if (!m_attached.syntax().isTargetingSameSchema(
        info->getSchemaForSourceDatabase().syntax())) {
        succeed = detach() && attach(info);
    }
    m_migratingInfo = nullptr;
    finalizeMigrationStatement();
    return succeed;
}

bool MigrateHandle::attach(const MigrationBaseInfo* info)
{
    WCTAssert(!isInTransaction());
    WCTAssert(!isPrepared());
    WCTAssert(m_attached.syntax().isMain());

    const StatementAttach& attach = info->getStatementForAttachingSchema();
    const Data& cipher = info->getSourceCipher();
    HandleStatement handleStatement(this);
    bool succeed = handleStatement.prepare(attach);
    if (succeed) {
        if (!cipher.empty()) {
            handleStatement.bindBLOB(cipher);
        }
        succeed = handleStatement.step();
        handleStatement.finalize();
    }
    if (succeed) {
        if (info->needRawSourceCipher()) {
            info->setRawSourceCipher(getRawCipherKey(info->getSchemaForSourceDatabase()));
        }
        m_attached = info->getSchemaForSourceDatabase();
    }
    return succeed;
}

bool MigrateHandle::detach()
{
    WCTAssert(!isInTransaction());
    WCTAssert(!isPrepared());

    bool succeed = true;
    if (!m_attached.syntax().isMain()) {
        succeed = execute(WCDB::StatementDetach().detach(m_attached));
        if (succeed) {
            m_attached = Schema::main();
        }
    }
    return succeed;
}

#pragma mark - Stepper
Optional<std::set<StringView>> MigrateHandle::getAllTables()
{
    Column name("name");
    Column type("type");
    StringView pattern
    = StringView::formatted("%s%%", Syntax::builtinTablePrefix.data());
    return getValues(StatementSelect()
                     .select(name)
                     .from(TableOrSubquery::master())
                     .where(type == "table" && name.notLike(pattern)),
                     0);
}

bool MigrateHandle::dropSourceTable(const MigrationInfo* info)
{
    WCTAssert(info != nullptr);
    bool succeed = false;
    if (reAttach(info)) {
        m_migratingInfo = info;
        succeed = execute(m_migratingInfo->getStatementForDroppingSourceTable());
    }
    return succeed;
}

Optional<bool> MigrateHandle::migrateRows(const MigrationInfo* info)
{
    WCTAssert(info != nullptr);
    auto exists = tableExists(info->getTable());
    if (!exists.succeed()) {
        return NullOpt;
    }

    if (!exists.value()) {
        return true;
    }

    if (m_migratingInfo != info) {
        if (!reAttach(info)) {
            return NullOpt;
        }
        m_migratingInfo = info;
    }

    if (!m_migrateStatement->isPrepared()
        && !m_migrateStatement->prepare(m_migratingInfo->getStatementForMigratingOneRow())) {
        return NullOpt;
    }

    if (!m_removeMigratedStatement->isPrepared()
        && !m_removeMigratedStatement->prepare(
        m_migratingInfo->getStatementForDeletingMigratedOneRow())) {
        return NullOpt;
    }

    double timeIntervalWithinTransaction = calculateTimeIntervalWithinTransaction();
    SteadyClock beforeTransaction = SteadyClock::now();
    Optional<bool> migrated;
    if (runTransaction([&migrated, &beforeTransaction, &timeIntervalWithinTransaction, this](
                       InnerHandle*) -> bool {
            double cost = 0;
            do {
                migrated = migrateRow();
                cost = SteadyClock::timeIntervalSinceSteadyClockToNow(beforeTransaction);
            } while (migrated.succeed() && !migrated.value()
                     && cost < timeIntervalWithinTransaction);
            timeIntervalWithinTransaction = cost;
            return migrated.succeed();
        })) {
        // update only if succeed
        double timeIntervalWholeTranscation
        = SteadyClock::timeIntervalSinceSteadyClockToNow(beforeTransaction);
        addSample(timeIntervalWithinTransaction, timeIntervalWholeTranscation);

        WCTAssert(migrated.succeed());
        return migrated;
    }
    return NullOpt;
}

Optional<bool> MigrateHandle::migrateRow()
{
    WCTAssert(m_migrateStatement->isPrepared() && m_removeMigratedStatement->isPrepared());
    WCTAssert(isInTransaction());
    Optional<bool> migrated;
    m_migrateStatement->reset();
    m_removeMigratedStatement->reset();
    if (m_migrateStatement->step()) {
        if (getChanges() != 0) {
            if (m_removeMigratedStatement->step()) {
                migrated = false;
            }
        } else {
            migrated = true;
        }
    }
    return migrated;
}

void MigrateHandle::finalizeMigrationStatement()
{
    m_migrateStatement->finalize();
    m_removeMigratedStatement->finalize();
}

#pragma mark - Sample
MigrateHandle::Sample::Sample()
: timeIntervalWithinTransaction(0), timeIntervalWholeTransaction(0)
{
}

void MigrateHandle::addSample(double timeIntervalWithinTransaction, double timeIntervalForWholeTransaction)
{
    WCTAssert(timeIntervalWithinTransaction > 0);
    WCTAssert(timeIntervalForWholeTransaction > 0);
    WCTAssert(m_samplePointing < numberOfSamples);
    WCTAssert(timeIntervalForWholeTransaction > timeIntervalWithinTransaction);

    Sample& sample = m_samples[m_samplePointing];
    sample.timeIntervalWithinTransaction = timeIntervalWithinTransaction;
    sample.timeIntervalWholeTransaction = timeIntervalForWholeTransaction;
    ++m_samplePointing;
    if (m_samplePointing >= numberOfSamples) {
        m_samplePointing = 0;
    }
}

double MigrateHandle::calculateTimeIntervalWithinTransaction() const
{
    double totalTimeIntervalWithinTransaction = 0;
    double totalTimeIntervalWholeTransaction = 0;
    for (const auto& sample : m_samples) {
        if (sample.timeIntervalWithinTransaction > 0
            && sample.timeIntervalWholeTransaction > 0) {
            totalTimeIntervalWithinTransaction += sample.timeIntervalWithinTransaction;
            totalTimeIntervalWholeTransaction += sample.timeIntervalWholeTransaction;
        }
    }
    double timeIntervalWithinTransaction = MigrateMaxExpectingDuration * totalTimeIntervalWithinTransaction
                                           / totalTimeIntervalWholeTransaction;
    if (timeIntervalWithinTransaction > MigrateMaxExpectingDuration
        || timeIntervalWithinTransaction <= 0 || std::isnan(timeIntervalWithinTransaction)) {
        timeIntervalWithinTransaction = MigrateMaxInitializeDuration;
    }
    return timeIntervalWithinTransaction;
}

#pragma mark - Info Initializer
bool MigrateHandle::attachSourceDatabase(const MigrationUserInfo& userInfo)
{
    return reAttach(&userInfo);
}

InnerHandle* MigrateHandle::getCurrentHandle()
{
    return this;
}

const StringView& MigrateHandle::getDatabasePath() const
{
    return getPath();
}

} // namespace WCDB
