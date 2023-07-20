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

#include "MigratingHandle.hpp"
#include "Assertion.hpp"
#include "MigrationInfo.hpp"
#include "StringView.hpp"

namespace WCDB {

#pragma mark - Initialize
MigratingHandle::MigratingHandle(Migration& migration)
: InnerHandle(), Migration::Binder(migration), m_createdNewViewInTransaction(false)
{
    Super::returnStatement(m_mainStatement);
    m_mainStatement = getStatement();
}

MigratingHandle::~MigratingHandle()
{
    returnStatement(m_mainStatement);
    m_mainStatement = nullptr;
}

#pragma mark - Meta

Optional<const MigrationInfo*> MigratingHandle::getBindingInfo(const UnsafeStringView& table)
{
    const MigrationInfo* boundInfo = getBoundInfo(table);
    if (boundInfo != nullptr) {
        return boundInfo;
    }
    Optional<const MigrationInfo*> result;

    startBinding();

    result = bindTable(table);

    bool needBinding = result.hasValue() && result.value() != nullptr;

    bool success = stopBinding(needBinding);
    if (needBinding && !success) {
        return Optional<const MigrationInfo*>();
    }

    return result;
}

Optional<std::set<StringView>>
MigratingHandle::getColumns(const Schema& schema, const UnsafeStringView& table)
{
    auto ret = Super::getColumns(schema, table);
    if (!ret.succeed()) {
        return ret;
    }
    if (!schema.syntax().isMain()) {
        return ret;
    }

    auto info = getBindingInfo(table);
    if (!info.succeed()) {
        return Optional<std::set<StringView>>();
    }
    if (info.value() == nullptr) {
        return ret;
    }
    auto sourceColumns = Super::getColumns(
    info.value()->getSchemaForSourceDatabase(), info.value()->getSourceTable());
    if (!sourceColumns.succeed()) {
        return NullOpt;
    }
    auto iterator = ret->begin();
    while (iterator != ret->end()) {
        if (sourceColumns->find(*iterator) == sourceColumns->end()) {
            iterator = ret->erase(iterator);
        } else {
            iterator++;
        }
    }
    return ret;
}

bool MigratingHandle::addColumn(const Schema& schema,
                                const UnsafeStringView& table,
                                const ColumnDef& column)
{
    if (!Super::addColumn(schema, table, column)) {
        return false;
    }

    if (!schema.syntax().isMain()) {
        return true;
    }

    auto info = getBindingInfo(table);
    if (!info.succeed()) {
        return false;
    }
    if (info.value() == nullptr) {
        return true;
    }

    return Super::addColumn(
    info.value()->getSchemaForSourceDatabase(), info.value()->getSourceTable(), column);
}

bool MigratingHandle::rebindUnionView(const UnsafeStringView& table, const Columns& columns)
{
    auto info = getBindingInfo(table);
    if (!info.succeed()) {
        return false;
    }
    if (info.value() == nullptr) {
        return true;
    }

    return runTransactionIfNotInTransaction([&](InnerHandle* handle) {
        HandleStatement handleStatement = HandleStatement(handle);
        bool succeed = handleStatement.prepare(MigrationInfo::getStatementForDroppingUnionedView(
                       info.value()->getUnionedView()))
                       && handleStatement.step();
        handleStatement.finalize();
        if (!succeed) {
            return false;
        }
        StatementCreateView createView
        = info.value()->getStatementForCreatingUnionedView(columns);
        succeed = handleStatement.prepare(createView) && handleStatement.step();
        handleStatement.finalize();
        return succeed;
    });
}

bool MigratingHandle::checkSourceTable(const UnsafeStringView& table,
                                       const UnsafeStringView& sourceTable)
{
    auto info = getBindingInfo(table);
    if (!info.succeed()) {
        return false;
    }
    if (info.value() == nullptr) {
        return false;
    }
    return info.value()->getSourceTable().compare(sourceTable) == 0;
}

bool MigratingHandle::attachDatabase(const MigrationBaseInfo* attachInfo)
{
    const StatementAttach& attach = attachInfo->getStatementForAttachingSchema();
    const Data& cipher = attachInfo->getSourceCipher();
    HandleStatement handleStatement(this);
    bool succeed = handleStatement.prepare(attach);
    if (succeed) {
        if (!cipher.empty()) {
            handleStatement.bindBLOB(cipher);
        }
        succeed = handleStatement.step();
        handleStatement.finalize();
    }
    if (succeed && attachInfo->needRawSourceCipher()) {
        attachInfo->setRawSourceCipher(
        getRawCipherKey(attachInfo->getSchemaForSourceDatabase()));
    }
    return succeed;
}

#pragma mark - Info Initializer
bool MigratingHandle::attachSourceDatabase(const MigrationUserInfo& userInfo)
{
    const Schema& schema = userInfo.getSchemaForSourceDatabase();
    if (!schema.syntax().isMain()) {
        auto optionalAttacheds
        = getValues(MigrationInfo::getStatementForSelectingDatabaseList(), 1);
        if (!optionalAttacheds.succeed()) {
            return false;
        }
        std::set<StringView>& attacheds = optionalAttacheds.value();
        if (attacheds.find(schema.getDescription()) == attacheds.end()) {
            if (!attachDatabase(&userInfo) || !trySynchronousTransactionAfterAttached()) {
                return false;
            }
        }
    }
    return true;
}

InnerHandle* MigratingHandle::getCurrentHandle()
{
    return this;
}

const StringView& MigratingHandle::getDatabasePath() const
{
    return getPath();
}

#pragma mark - Binder
bool MigratingHandle::rebindViews(const StringViewMap<const MigrationInfo*>& migratings)
{
    StringViewMap<const MigrationInfo*> views2MigratingInfos;
    for (const auto& iter : migratings) {
        const MigrationInfo* info = iter.second;
        WCTAssert(info != nullptr);
        views2MigratingInfos.emplace(info->getUnionedView(), info);
    }

    // get existing unioned views
    auto exists = tableExists(Schema::temp(), Syntax::masterTable);
    if (!exists.succeed()) {
        return false;
    }
    if (exists.value()) {
        auto existingViews
        = getValues(MigrationInfo::getStatementForSelectingUnionedView(), 0);
        if (!existingViews.succeed()) {
            return false;
        }

        for (const auto& existingView : existingViews.value()) {
            WCTAssert(existingView.hasPrefix(MigrationInfo::getUnionedViewPrefix()));
            auto iter = views2MigratingInfos.find(existingView);
            if (iter != views2MigratingInfos.end()) {
                // it is already created
                views2MigratingInfos.erase(iter);
            } else {
                // it is no longer needed
                if (!executeStatement(
                    MigrationInfo::getStatementForDroppingUnionedView(existingView))) {
                    return false;
                }
            }
        }
    }

    bool hasNewView = false;
    // create all needed views
    for (const auto& iter : views2MigratingInfos) {
        if (!executeStatement(iter.second->getStatementForCreatingUnionedView())) {
            return false;
        }
        hasNewView = true;
    }
    if (hasNewView && isInTransaction()) {
        m_createdNewViewInTransaction = true;
    }
    return true;
}

bool MigratingHandle::rebindSchemas(const StringViewMap<const MigrationInfo*>& migratings)
{
    StringViewMap<const MigrationInfo*> schemas2MigratingInfos;
    for (const auto& iter : migratings) {
        const MigrationInfo* info = iter.second;
        WCTAssert(info != nullptr);
        if (info->isCrossDatabase()) {
            schemas2MigratingInfos.emplace(
            info->getSchemaForSourceDatabase().getDescription(), info);
        }
    }

    auto existingSchemas
    = getValues(MigrationInfo::getStatementForSelectingDatabaseList(), 1);
    if (!existingSchemas.succeed()) {
        return false;
    }

    for (const auto& existingSchema : existingSchemas.value()) {
        if (existingSchema.hasPrefix(MigrationDatabaseInfo::getSchemaPrefix())) {
            auto iter = schemas2MigratingInfos.find(existingSchema);
            if (iter != schemas2MigratingInfos.end()) {
                // it is already attached
                schemas2MigratingInfos.erase(iter);
            } else {
                if (!isInTransaction()) {
                    // it is not longer needed
                    if (!executeStatement(
                        MigrationInfo::getStatementForDetachingSchema(existingSchema))) {
                        return false;
                    }
                }
            }
        }
    }
    bool attached = false;
    // attach all needed schemas
    for (const auto& iter : schemas2MigratingInfos) {
        if (!attachDatabase(iter.second)) {
            return false;
        }
        attached = true;
    }
    if (attached) {
        return trySynchronousTransactionAfterAttached();
    }
    return true;
}

bool MigratingHandle::commitTransaction()
{
    bool ret = Super::commitTransaction();
    if (ret && !isInTransaction()) {
        m_createdNewViewInTransaction = false;
    }
    return ret;
}

void MigratingHandle::rollbackTransaction()
{
    Super::rollbackTransaction();
    if (m_createdNewViewInTransaction) {
        setNeedRebind();
        m_createdNewViewInTransaction = false;
    }
}

bool MigratingHandle::bindInfos(const StringViewMap<const MigrationInfo*>& migratings)
{
    return rebindViews(migratings) && rebindSchemas(migratings);
}

bool MigratingHandle::trySynchronousTransactionAfterAttached()
{
    bool succeed = true;
    if (isInTransaction()) {
        markErrorAsIgnorable(Error::Code::Error);
        static const StatementBegin* s_synchronousTransaction
        = new StatementBegin(StatementBegin().beginImmediate());
        succeed = executeStatement(*s_synchronousTransaction);
        WCTAssert(!succeed);
        if (!succeed && getError().isIgnorable()) {
            succeed = true;
        }
        markErrorAsUnignorable();
    }
    return succeed;
}

#pragma mark - Statement
void MigratingHandle::finalize()
{
    Super::finalize();
    for (auto iter = m_migratingHandleStatements.begin();
         iter != m_migratingHandleStatements.end();
         ++iter) {
        if (iter->isPrepared()) {
            return;
        }
    }
    stopReferenced();
}

HandleStatement* MigratingHandle::getStatement()
{
    m_migratingHandleStatements.push_back(MigratingHandleStatement(this));
    m_migratingHandleStatements.back().enableAutoAddColumn();
    return &m_migratingHandleStatements.back();
}

void MigratingHandle::returnStatement(HandleStatement* handleStatement)
{
    if (handleStatement != nullptr) {
        for (auto iter = m_migratingHandleStatements.begin();
             iter != m_migratingHandleStatements.end();
             ++iter) {
            if (&(*iter) == handleStatement) {
                m_migratingHandleStatements.erase(iter);
                return;
            }
        }
        WCTAssert(false);
    }
}

void MigratingHandle::finalizeStatements()
{
    for (auto& handleStatement : m_migratingHandleStatements) {
        handleStatement.finalize();
    }
    stopReferenced();
}

void MigratingHandle::returnAllPreparedStatement()
{
    Super::returnAllPreparedStatement();
    if (!m_mainStatement->isPrepared()) {
        stopReferenced();
    }
}

void MigratingHandle::resetAllStatements()
{
    for (auto& handleStatement : m_migratingHandleStatements) {
        if (!handleStatement.isPrepared()) continue;
        handleStatement.reset();
    }
}

} //namespace WCDB
