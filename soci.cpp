//
// Copyright (C) 2004-2006 Maciej Sobczak, Stephen Hutton
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#include "soci.h"

#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif

using namespace SOCI;
using namespace SOCI::details;

SOCIError::SOCIError(std::string const & msg)
    : std::runtime_error(msg)
{
}

Session::Session(std::string const & backEndName,
    std::string const & connectString)
    : once(this), prepare(this)
{
    BackEndFactory const *factory = theBEFRegistry().find(backEndName);
    backEnd_ = factory->makeSession(connectString);
}

Session::~Session()
{
    delete backEnd_;
}

void Session::begin()
{
    backEnd_->begin();
}

void Session::commit()
{
    backEnd_->commit();
}

void Session::rollback()
{
    backEnd_->rollback();
}

StatementBackEnd * Session::makeStatementBackEnd()
{
    return backEnd_->makeStatementBackEnd();
}

RowIDBackEnd * Session::makeRowIDBackEnd()
{
    return backEnd_->makeRowIDBackEnd();
}

BLOBBackEnd * Session::makeBLOBBackEnd()
{
    return backEnd_->makeBLOBBackEnd();
}

Statement::Statement(Session &s)
    : session_(s), row_(0), fetchSize_(1), initialFetchSize_(1)
{
    backEnd_ = s.makeStatementBackEnd();
}

Statement::Statement(PrepareTempType const &prep)
    : session_(*prep.getPrepareInfo()->session_),
      row_(0), fetchSize_(1)
{
    backEnd_ = session_.makeStatementBackEnd();

    RefCountedPrepareInfo *prepInfo = prep.getPrepareInfo();

    // take all bind/define info
    intos_.swap(prepInfo->intos_);
    uses_.swap(prepInfo->uses_);

    // allocate handle
    alloc();

    // prepare the statement
    query_ = prepInfo->getQuery();
    prepare(prepInfo->getQuery());

    defineAndBind();
}

Statement::~Statement()
{
    cleanUp();
}

void Statement::alloc()
{
    backEnd_->alloc();
}

void Statement::bind(Values& values)
{
    size_t cnt = 0;

    try
    {
        for(std::vector<details::StandardUseType*>::iterator it = 
            values.uses_.begin(); it != values.uses_.end(); ++it)
        {
            // only bind those variables which are actually
            // referenced in the statement
            const std::string name = ":" + (*it)->getName();

            size_t pos = query_.find(name);
            if (pos != std::string::npos)
            {
                const char nextChar = query_[pos + name.size()];
                if(nextChar == ' ' || nextChar == ',' ||
                   nextChar == '\0' || nextChar == ')')
                {
                    int position = static_cast<int>(uses_.size());
                    (*it)->bind(*this, position);
                    uses_.push_back(*it);
                    indicators_.push_back(values.indicators_[cnt]);
                }
                else
                {
                    values.addUnused(*it, values.indicators_[cnt]);
                }
            }
            else
            {
                values.addUnused(*it, values.indicators_[cnt]);
            }

            cnt++;
        }
    }
    catch(...)
    {
        for(size_t i = ++cnt; i != values.uses_.size(); ++i)
        {            
            values.addUnused(uses_[i], values.indicators_[i]);
        }
        throw; 
    }
}


void Statement::exchange(IntoTypePtr const &i)
{
    intos_.push_back(i.get());
    i.release();
}

void Statement::exchange(UseTypePtr const &u)
{
    uses_.push_back(u.get());
    u.release();
}

void Statement::cleanUp()
{
    // deallocate all bind and define objects
    for (std::size_t i = intos_.size(); i != 0; --i)
    {
        intos_[i - 1]->cleanUp();
        delete intos_[i - 1];
        intos_.resize(i - 1);
    }

    for (std::size_t i = uses_.size(); i != 0; --i)
    {
        uses_[i - 1]->cleanUp();
        delete uses_[i - 1];
        uses_.resize(i - 1);
    }

    for (std::size_t i = 0; i != indicators_.size(); ++i)
    {
        delete indicators_[i];
        indicators_[i] = NULL;
    }

    if (backEnd_ != NULL)
    {
        backEnd_->cleanUp();
        delete backEnd_;
        backEnd_ = NULL;
    }
}

void Statement::prepare(std::string const &query)
{
    query_ = query;
    backEnd_->prepare(query);
}

void Statement::defineAndBind()
{
    int definePosition = 1;

    // check intos_.size() on each iteration
    // because IntoType<Row> can resize it
    for (std::size_t i = 0; i != intos_.size(); ++i)
    {
        intos_[i]->define(*this, definePosition);
    }

    int bindPosition = 1;
    std::size_t const usize = uses_.size();
    for (std::size_t i = 0; i != usize; ++i)
    {
        uses_[i]->bind(*this, bindPosition);
    }
}

void Statement::unDefAndBind()
{
    for (std::size_t i = intos_.size(); i != 0; --i)
    {
        intos_[i - 1]->cleanUp();
    }

    for (std::size_t i = uses_.size(); i != 0; --i)
    {
        uses_[i - 1]->cleanUp();
    }
}

bool Statement::execute(bool withDataExchange)
{
    initialFetchSize_ = intosSize();
    fetchSize_ = initialFetchSize_;

    std::size_t bindSize = usesSize();

    if (bindSize > 1 && fetchSize_ > 1)
    {
        throw SOCIError(
             "Bulk insert/update and bulk select not allowed in same query");
    }

    int num = 0;
    if (withDataExchange)
    {
        num = 1;

        preFetch();
        preUse();

        if (static_cast<int>(fetchSize_) > num)
        {
            num = static_cast<int>(fetchSize_);
        }
        if (static_cast<int>(bindSize) > num)
        {
            num = static_cast<int>(bindSize);
        }
    }

    StatementBackEnd::execFetchResult res = backEnd_->execute(num);

    bool gotData = false;

    if (res == StatementBackEnd::eSuccess)
    {
        // the "success" means that the statement executed correctly
        // and for select statement this also means that some rows were read

        if (num > 0)
        {
            gotData = true;

            // ensure into vectors have correct size
            resizeIntos(static_cast<std::size_t>(num));
        }
    }
    else // res == eNoData
    {
        // the "no data" means that the end-of-rowset condition was hit
        // but still some rows might have been read (the last bunch of rows)
        // it can also mean that the statement did not produce any results

        gotData = fetchSize_ > 1 ? resizeIntos() : false;
    }

    if (num > 0)
    {
        postFetch(gotData, false);
        postUse(gotData);
    }

    return gotData;
}

bool Statement::fetch()
{
    if (fetchSize_ == 0)
    {
        return false;
    }

    bool gotData = false;

    // vectors might have been resized between fetches
    std::size_t newFetchSize = intosSize();
    if (newFetchSize > initialFetchSize_)
    {
        // this is not allowed, because most likely caused reallocation
        // of the vector - this would require complete re-bind

        throw SOCIError(
            "Increasing the size of the output vector is not supported.");
    }
    else if (newFetchSize == 0)
    {
        return false;
    }
    else
    {
        // the output vector was downsized or remains the same as before
        fetchSize_ = newFetchSize;
    }

    StatementBackEnd::execFetchResult res =
        backEnd_->fetch(static_cast<int>(fetchSize_));
    if (res == StatementBackEnd::eSuccess)
    {
        // the "success" means that some number of rows was read
        // and that it is not yet the end-of-rowset (there are more rows)

        gotData = true;

        // ensure into vectors have correct size
        resizeIntos(fetchSize_);
    }
    else // res == eNoData
    {
        // end-of-rowset condition

        if (fetchSize_ > 1)
        {
            // but still the last bunch of rows might have been read
            gotData = resizeIntos();
            fetchSize_ = 0;
        }
        else
        {
            gotData = false;
        }
    }

    postFetch(gotData, true);
    return gotData;
}

std::size_t Statement::intosSize()
{
    std::size_t intosSize = 0;
    std::size_t const isize = intos_.size();
    for (std::size_t i = 0; i != isize; ++i)
    {
        if (i==0)
        {
            intosSize = intos_[i]->size();
            if (intosSize == 0)
            {
                 // this can happen only for vectors
                 throw SOCIError("Vectors of size 0 are not allowed.");
            }
        }
        else if (intosSize != intos_[i]->size())
        {
            std::ostringstream msg;
            msg << "Bind variable size mismatch (into["
                << static_cast<unsigned long>(i) << "] has size "
                << static_cast<unsigned long>(intos_[i]->size())
                << ", into[0] has size "
                << static_cast<unsigned long>(intosSize);
            throw SOCIError(msg.str());
        }
    }
    return intosSize;
}

std::size_t Statement::usesSize()
{
    std::size_t usesSize = 0;
    std::size_t const usize = uses_.size();
    for (std::size_t i = 0; i != usize; ++i)
    {
        if (i==0)
        {
            usesSize = uses_[i]->size();
            if (usesSize == 0)
            {
                 // this can happen only for vectors
                 throw SOCIError("Vectors of size 0 are not allowed.");
            }
        }
        else if (usesSize != uses_[i]->size())
        {
            std::ostringstream msg;
            msg << "Bind variable size mismatch (use["
                << static_cast<unsigned long>(i) << "] has size "
                << static_cast<unsigned long>(uses_[i]->size())
                << ", use[0] has size "
                << static_cast<unsigned long>(usesSize);
            throw SOCIError(msg.str());
        }
    }
    return usesSize;
}

bool Statement::resizeIntos(std::size_t upperBound)
{
    std::size_t rows = backEnd_->getNumberOfRows();
    if (upperBound != 0 && upperBound < rows)
    {
        rows = upperBound;
    }

    std::size_t const isize = intos_.size();
    for (std::size_t i = 0; i != isize; ++i)
    {
        intos_[i]->resize(rows);
    }

    return rows > 0 ? true : false;
}

void Statement::preFetch()
{
    std::size_t const isize = intos_.size();
    for (std::size_t i = 0; i != isize; ++i)
    {
        intos_[i]->preFetch();
    }
}

void Statement::preUse()
{
    std::size_t const usize = uses_.size();
    for (std::size_t i = 0; i != usize; ++i)
    {
        uses_[i]->preUse();
    }
}

void Statement::postFetch(bool gotData, bool calledFromFetch)
{
    // iterate in reverse order here in case the first item
    // is an IntoType<Row> (since it depends on the other IntoTypes)
    for (std::size_t i = intos_.size(); i != 0; --i)
    {
        intos_[i-1]->postFetch(gotData, calledFromFetch);
    }
}

void Statement::postUse(bool gotData)
{ 
    // iterate in reverse order here in case the first item
    // is an UseType<Values> (since it depends on the other UseTypes)
    for (std::size_t i = uses_.size(); i != 0; --i)
    {
        uses_[i-1]->postUse(gotData);
    }
}

details::StandardIntoTypeBackEnd * Statement::makeIntoTypeBackEnd()
{
    return backEnd_->makeIntoTypeBackEnd();
}

details::StandardUseTypeBackEnd * Statement::makeUseTypeBackEnd()
{
    return backEnd_->makeUseTypeBackEnd();
}

details::VectorIntoTypeBackEnd * Statement::makeVectorIntoTypeBackEnd()
{
    return backEnd_->makeVectorIntoTypeBackEnd();
}

details::VectorUseTypeBackEnd * Statement::makeVectorUseTypeBackEnd()
{
    return backEnd_->makeVectorUseTypeBackEnd();
}

// Map eDataTypes to stock types for dynamic result set support
namespace SOCI
{

template<>
void Statement::bindInto<eString>()
{
    intoRow<std::string>();
}

template<>
void Statement::bindInto<eDouble>()
{
    intoRow<double>();
}

template<>
void Statement::bindInto<eInteger>()
{
    intoRow<int>();
}

template<>
void Statement::bindInto<eUnsignedLong>()
{
    intoRow<unsigned long>();
}

template<>
void Statement::bindInto<eDate>()
{
    intoRow<std::tm>();
}

} //namespace SOCI

void Statement::describe()
{
    int numcols = backEnd_->prepareForDescribe();

    for (int i = 1; i <= numcols; ++i)
    {
        eDataType dtype;
        std::string columnName;

        backEnd_->describeColumn(i, dtype, columnName);

        ColumnProperties props;
        props.setName(columnName);

        props.setDataType(dtype);
        switch(dtype)
        {
        case eString:
            bindInto<eString>();
            break;
        case eDouble:
            bindInto<eDouble>();
            break;
        case eInteger:
            bindInto<eInteger>();
            break;
        case eUnsignedLong:
            bindInto<eUnsignedLong>();
            break;
        case eDate:
            bindInto<eDate>();
            break;
        default:
            std::ostringstream msg;
            msg << "db column type " << dtype
                <<" not supported for dynamic selects"<<std::endl;
            throw SOCIError(msg.str());
        }
        row_->addProperties(props);
    }
}

std::string Statement::rewriteForProcedureCall(std::string const &query)
{
    return backEnd_->rewriteForProcedureCall(query);
}

Procedure::Procedure(PrepareTempType const &prep)
    : Statement(*prep.getPrepareInfo()->session_)
{
    RefCountedPrepareInfo *prepInfo = prep.getPrepareInfo();

    // take all bind/define info
    intos_.swap(prepInfo->intos_);
    uses_.swap(prepInfo->uses_);

    // allocate handle
    alloc();

    // prepare the statement
    prepare(rewriteForProcedureCall(prepInfo->getQuery()));

    defineAndBind();
}

RefCountedStatement::~RefCountedStatement()
{
    try
    {
        st_.alloc();
        st_.prepare(query_.str());
        st_.defineAndBind();
        st_.execute(true);
    }
    catch (...)
    {
        st_.cleanUp();
        throw;
    }

    st_.cleanUp();
}

void RefCountedPrepareInfo::exchange(IntoTypePtr const &i)
{
    intos_.push_back(i.get());
    i.release();
}

void RefCountedPrepareInfo::exchange(UseTypePtr const &u)
{
    uses_.push_back(u.get());
    u.release();
}

RefCountedPrepareInfo::~RefCountedPrepareInfo()
{
    // deallocate all bind and define objects
    for (std::size_t i = intos_.size(); i > 0; --i)
    {
        delete intos_[i - 1];
        intos_.resize(i - 1);
    }

    for (std::size_t i = uses_.size(); i > 0; --i)
    {
        delete uses_[i - 1];
        uses_.resize(i - 1);
    }
}

OnceTempType::OnceTempType(Session &s)
    : rcst_(new RefCountedStatement(s))
{
}

OnceTempType::OnceTempType(OnceTempType const &o)
    :rcst_(o.rcst_)
{
    rcst_->incRef();
}

OnceTempType & OnceTempType::operator=(OnceTempType const &o)
{
    o.rcst_->incRef();
    rcst_->decRef();
    rcst_ = o.rcst_;

    return *this;
}

OnceTempType::~OnceTempType()
{
    rcst_->decRef();
}

OnceTempType & OnceTempType::operator,(IntoTypePtr const &i)
{
    rcst_->exchange(i);
    return *this;
}

OnceTempType & OnceTempType::operator,(UseTypePtr const &u)
{
    rcst_->exchange(u);
    return *this;
}

PrepareTempType::PrepareTempType(Session &s)
    : rcpi_(new RefCountedPrepareInfo(s))
{
}

PrepareTempType::PrepareTempType(PrepareTempType const &o)
    :rcpi_(o.rcpi_)
{
    rcpi_->incRef();
}

PrepareTempType & PrepareTempType::operator=(PrepareTempType const &o)
{
    o.rcpi_->incRef();
    rcpi_->decRef();
    rcpi_ = o.rcpi_;

    return *this;
}

PrepareTempType::~PrepareTempType()
{
    rcpi_->decRef();
}

PrepareTempType & PrepareTempType::operator,(IntoTypePtr const &i)
{
    rcpi_->exchange(i);
    return *this;
}

PrepareTempType & PrepareTempType::operator,(UseTypePtr const &u)
{
    rcpi_->exchange(u);
    return *this;
}

void Row::addProperties(ColumnProperties const &cp)
{
    columns_.push_back(cp);
    index_[cp.getName()] = columns_.size() - 1;
}

std::size_t Row::size() const
{
    return holders_.size();
}

eIndicator Row::indicator(std::size_t pos) const
{
    assert(indicators_.size() >= static_cast<std::size_t>(pos + 1));
    return *indicators_[pos];
}

eIndicator Row::indicator(std::string const &name) const
{
    return indicator(findColumn(name));
}

ColumnProperties const & Row::getProperties(std::size_t pos) const
{
    assert(columns_.size() >= pos + 1);
    return columns_[pos];
}

ColumnProperties const & Row::getProperties(std::string const &name) const
{
    return getProperties(findColumn(name));
}

std::size_t Row::findColumn(std::string const &name) const
{
    std::map<std::string, std::size_t>::const_iterator it = index_.find(name);
    if (it == index_.end())
    {
        std::ostringstream msg;
        msg << "Column '" << name << "' not found";
        throw SOCIError(msg.str());
    }

    return it->second;
}

Row::~Row()
{
    std::size_t const hsize = holders_.size();
    for(std::size_t i = 0; i != hsize; ++i)
    {
        delete holders_[i];
        delete indicators_[i];
    }
}

eIndicator Values::indicator(std::size_t pos) const
{
    return row_->indicator(pos);
}

eIndicator Values::indicator(std::string const &name) const
{
    return row_->indicator(name);
}


// implementation of into and use types

// standard types

StandardIntoType::~StandardIntoType()
{
    delete backEnd_;
}

void StandardIntoType::define(Statement &st, int &position)
{
    backEnd_ = st.makeIntoTypeBackEnd();
    backEnd_->defineByPos(position, data_, type_);
}

void StandardIntoType::preFetch()
{
    backEnd_->preFetch();
}

void StandardIntoType::postFetch(bool gotData, bool calledFromFetch)
{
    backEnd_->postFetch(gotData, calledFromFetch, ind_);

    if (gotData)
    {
        convertFrom();
    }
}

void StandardIntoType::cleanUp()
{
    // backEnd_ might be NULL if IntoType<Row> was used
    if (backEnd_ != NULL)
    {
        backEnd_->cleanUp();
    }
}

StandardUseType::~StandardUseType()
{
    delete backEnd_;
}

void StandardUseType::bind(Statement &st, int &position)
{
    backEnd_ = st.makeUseTypeBackEnd();
    if (name_.empty())
    {
        backEnd_->bindByPos(position, data_, type_);
    }
    else
    {
        backEnd_->bindByName(name_, data_, type_);
    }
}

void StandardUseType::preUse()
{
    convertTo();

    backEnd_->preUse(ind_);
}

void StandardUseType::postUse(bool gotData)
{
    backEnd_->postUse(gotData, ind_);

    convertFrom();
}

void StandardUseType::cleanUp()
{
    backEnd_->cleanUp();
}

// vector based types

VectorIntoType::~VectorIntoType()
{
    delete backEnd_;
}

void VectorIntoType::define(Statement &st, int &position)
{
    backEnd_ = st.makeVectorIntoTypeBackEnd();
    backEnd_->defineByPos(position, data_, type_);
}

void VectorIntoType::preFetch()
{
    backEnd_->preFetch();
}

void VectorIntoType::postFetch(bool gotData, bool /* calledFromFetch */)
{
    if (indVec_ != NULL && indVec_->empty() == false)
    {
        assert(indVec_->empty() == false);
        backEnd_->postFetch(gotData, &(*indVec_)[0]);
    }
    else
    {
        backEnd_->postFetch(gotData, NULL);
    }

    if(gotData)
    {
        convertFrom();
    }
}

void VectorIntoType::resize(std::size_t sz)
{
    if (indVec_ != NULL)
    {
        indVec_->resize(sz);
    }

    backEnd_->resize(sz);
}

std::size_t VectorIntoType::size() const
{
    return backEnd_->size();
}

void VectorIntoType::cleanUp()
{
    if (backEnd_ != NULL)
    {
        backEnd_->cleanUp();
    }
}

VectorUseType::~VectorUseType()
{
    delete backEnd_;
}

void VectorUseType::bind(Statement &st, int &position)
{
    backEnd_ = st.makeVectorUseTypeBackEnd();
    if (name_.empty())
    {
        backEnd_->bindByPos(position, data_, type_);
    }
    else
    {
        backEnd_->bindByName(name_, data_, type_);
    }
}

void VectorUseType::preUse()
{
    convertTo();

    backEnd_->preUse(ind_);
}

std::size_t VectorUseType::size() const
{
    return backEnd_->size();
}

void VectorUseType::cleanUp()
{
    if (backEnd_ != NULL)
    {
        backEnd_->cleanUp();
    }
}


// basic BLOB operations

BLOB::BLOB(Session &s)
{
    backEnd_ = s.makeBLOBBackEnd();
}

BLOB::~BLOB()
{
    delete backEnd_;
}

std::size_t BLOB::getLen()
{
    return backEnd_->getLen();
}

std::size_t BLOB::read(std::size_t offset, char *buf, std::size_t toRead)
{
    return backEnd_->read(offset, buf, toRead);
}

std::size_t BLOB::write(
    std::size_t offset, char const *buf, std::size_t toWrite)
{
    return backEnd_->write(offset, buf, toWrite);
}

std::size_t BLOB::append(char const *buf, std::size_t toWrite)
{
    return backEnd_->append(buf, toWrite);
}

void BLOB::trim(std::size_t newLen)
{
    backEnd_->trim(newLen);
}

// ROWID support

RowID::RowID(Session &s)
{
    backEnd_ = s.makeRowIDBackEnd();
}

RowID::~RowID()
{
    delete backEnd_;
}


// back-end factory registry implementation

void BackEndFactoryRegistry::registerMe(
    std::string const &beName, BackEndFactory const *f)
{
    registry_[beName] = f;
}

BackEndFactory const *
BackEndFactoryRegistry::find(std::string const &beName) const
{
    std::map<std::string, BackEndFactory const *>::const_iterator it
        = registry_.find(beName);

    if (it == registry_.end())
    {
        std::string msg("Back-end for ");
        msg += beName;
        msg += " not found.";
        throw SOCIError(msg);
    }
    else
    {
        return it->second;
    }
}

BackEndFactoryRegistry & details::theBEFRegistry()
{
    static BackEndFactoryRegistry registry;
    return registry;
}
