/* 
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <tbb/mutex.h>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/tuple/tuple.hpp>
#include "base/util.h"
#include "base/logging.h"
#include "base/address_util.h"
#include <tbb/atomic.h>
#include <cstdlib>
#include <cerrno>
#include <utility>
#include "hiredis/hiredis.h"
#include "hiredis/boostasio.hpp"
#include <list>
#include <contrail-collector/redis_connection.h>
#include "base/work_pipeline.h"
#include "QEOpServerProxy.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "query.h"
#include "analytics_types.h"
#include "stats_select.h"
#include <base/connection_info.h>

using std::list;
using std::string;
using std::vector;
using std::map;
using boost::assign::list_of;
using boost::ptr_map;
using boost::nullable;
using boost::tuple;
using boost::shared_ptr;
using boost::scoped_ptr;
using std::pair;
using std::auto_ptr;
using std::make_pair;
using process::ConnectionState;
using process::ConnectionType;
using process::ConnectionStatus;
using process::Endpoint;

extern RedisAsyncConnection * rac_alloc(EventManager *, const std::string & ,unsigned short,
RedisAsyncConnection::ClientConnectCbFn ,
RedisAsyncConnection::ClientDisconnectCbFn,
const bool, const std::string &, const std::string &, const std::string &);

extern RedisAsyncConnection * rac_alloc_nocheck(EventManager *, const std::string & ,unsigned short,
RedisAsyncConnection::ClientConnectCbFn ,
RedisAsyncConnection::ClientDisconnectCbFn,
const bool, const std::string &, const std::string &, const std::string &);

SandeshTraceBufferPtr QeTraceBuf(SandeshTraceBufferCreate(QE_TRACE_BUF, 10000));

struct RawResultT {
    QEOpServerProxy::QPerfInfo perf;
    shared_ptr<QEOpServerProxy::BufferT> res;
    shared_ptr<QEOpServerProxy::OutRowMultimapT>  mres;
    shared_ptr<WhereResultT> wres;
};

typedef pair<redisReply,vector<string> > RedisT;

bool RedisAsyncArgCommand(RedisAsyncConnection * rac,
            void *rpi, const vector<string>& args) {

    return rac->RedisAsyncArgCmd(rpi, args);
}

class QEOpServerProxy::QEOpServerImpl {
public:
    typedef std::vector<std::string> QEOutputT;

    struct Input {
        int redis_host_idx;
        int cnum;
        string hostname;
        QueryEngine::QueryParams qp;
        vector<uint64_t> chunk_size;
        uint32_t wterms;
        bool need_merge;
        bool map_output;
        string where;
        string select;
        string post;
        uint64_t time_period;
        string table;
        uint32_t max_rows;
        tbb::atomic<uint32_t> chunk_q;
        tbb::atomic<uint32_t> total_rows;
    };

    void JsonInsert(std::vector<query_column> &columns,
            contrail_rapidjson::Document& dd,
            std::pair<const string,string> * map_it) {

        bool found = false;
        for (size_t j = 0; j < columns.size(); j++)
        {
            if ((0 == map_it->first.compare(0,5,string("COUNT")))) {
                contrail_rapidjson::Value val(contrail_rapidjson::kNumberType);
                unsigned long num = 0;
                stringToInteger(map_it->second, num);
                val.SetUint64(num);
                contrail_rapidjson::Value vk;
                dd.AddMember(vk.SetString(map_it->first.c_str(), dd.GetAllocator()), val, dd.GetAllocator());
                found = true;
            } else if (columns[j].name == map_it->first) {
                if (map_it->second.length() == 0) {
                    contrail_rapidjson::Value val(contrail_rapidjson::kNullType);
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(map_it->first.c_str(), dd.GetAllocator()), val, dd.GetAllocator());
                    found = true;
                    continue;
                }

                // find out type and convert
                if (columns[j].datatype == "string" || 
                    columns[j].datatype == "uuid")
                {
                    contrail_rapidjson::Value val(contrail_rapidjson::kStringType);
                    val.SetString(map_it->second.c_str(), dd.GetAllocator());
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(map_it->first.c_str(), dd.GetAllocator()), val, dd.GetAllocator());
                } else if (columns[j].datatype == "ipaddr") {
                    contrail_rapidjson::Value val(contrail_rapidjson::kStringType);
                   val.SetString(map_it->second.c_str(),
                                 dd.GetAllocator());
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(map_it->first.c_str(), dd.GetAllocator()), val, dd.GetAllocator());

                } else if (columns[j].datatype == "double") {
                    contrail_rapidjson::Value val(contrail_rapidjson::kNumberType);
                    double dval = (double) strtod(map_it->second.c_str(), NULL);
                    val.SetDouble(dval);
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(map_it->first.c_str(), dd.GetAllocator()), val, dd.GetAllocator());
                } else {
                    contrail_rapidjson::Value val(contrail_rapidjson::kNumberType);
                    unsigned long num = 0;
                    stringToInteger(map_it->second, num);
                    val.SetUint64(num);
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(map_it->first.c_str(), dd.GetAllocator()), val, dd.GetAllocator());
                }
                found = true;
            }
        }
        assert(found);    
    }

    void QueryJsonify(const string& table, bool map_output,
        const BufferT* raw_res, const OutRowMultimapT* raw_mres, QEOutputT* raw_json) {

        vector<OutRowT>::iterator res_it;

        std::vector<query_column>  columns;

        if (!table.size()) return;
        bool found = false;
        for(size_t i = 0; i < g_viz_constants._TABLES.size(); i++)
        {
            if (g_viz_constants._TABLES[i].name == table) {
                found = true;
                columns = g_viz_constants._TABLES[i].schema.columns;
            }
        }
        if (!found) {
            found = true;
            columns = g_viz_constants._OBJECT_TABLE_SCHEMA.columns;
        }
        assert(found || map_output);

        if (map_output) {
            OutRowMultimapT::const_iterator mres_it;
            for (mres_it = raw_mres->begin(); mres_it != raw_mres->end(); ++mres_it) {
                string jstr;
                StatsSelect::Jsonify(table, mres_it->second.first, mres_it->second.second, jstr);
                raw_json->push_back(jstr);
            }
        } else {
            QEOpServerProxy::BufferT* raw_result = 
                const_cast<QEOpServerProxy::BufferT*>(raw_res);
            QEOpServerProxy::BufferT::iterator res_it;
            for (res_it = raw_result->begin(); res_it != raw_result->end(); ++res_it) {
                std::map<std::string, std::string>::iterator map_it;
                contrail_rapidjson::Document dd;
                dd.SetObject();

                for (map_it = (*res_it).first.begin(); 
                     map_it != (*res_it).first.end(); ++map_it) {
                    // search for column name in the schema
                    JsonInsert(columns, dd, &(*map_it));
                }
                contrail_rapidjson::StringBuffer sb;
                contrail_rapidjson::Writer<contrail_rapidjson::StringBuffer> writer(sb);
                dd.Accept(writer);
                raw_json->push_back(sb.GetString());
            }
        }        
    }


    void QECallback(void * qid, QPerfInfo qperf,
            auto_ptr<std::vector<query_result_unit_t> > res) {

        RawResultT* raw(new RawResultT);
        raw->perf = qperf;
        raw->wres = res;

        ExternalProcIf<RawResultT> * rpi = NULL;
        if (qid)
            rpi = reinterpret_cast<ExternalProcIf<RawResultT> *>(qid);

        if (rpi) {
            auto_ptr<RawResultT> rp(raw);
            rpi->Response(rp);
        }
    }

    void QECallback(void * qid, QPerfInfo qperf, auto_ptr<QEOpServerProxy::BufferT> res, 
            auto_ptr<QEOpServerProxy::OutRowMultimapT> mres) {

        RawResultT* raw(new RawResultT);
        raw->perf = qperf;
        raw->res = res;
        raw->mres = mres;

        ExternalProcIf<RawResultT> * rpi = NULL;
        if (qid)
            rpi = reinterpret_cast<ExternalProcIf<RawResultT> *>(qid);

        if (rpi) {
            auto_ptr<RawResultT> rp(raw);
            rpi->Response(rp);
        }
    }
   
    struct Stage0Out {
        Input inp;
        bool ret_code;
        vector<QPerfInfo> ret_info;
        vector<uint32_t> chunk_merge_time;
        shared_ptr<BufferT> result;
        shared_ptr<OutRowMultimapT> mresult;
        vector<shared_ptr<WhereResultT> > welem;
        shared_ptr<WhereResultT> wresult;
        uint32_t current_chunk;
    };

    ExternalBase::Efn QueryExec(uint32_t inst, const vector<RawResultT*> & exts,
            const Input & inp, Stage0Out & res) { 
        uint32_t step = exts.size();

        if (!step) {
            res.inp = inp;
            res.ret_code = true;
         
            if (inp.map_output)
                res.mresult = shared_ptr<OutRowMultimapT>(new OutRowMultimapT());
            else
                res.result = shared_ptr<BufferT>(new BufferT());

            res.wresult = shared_ptr<WhereResultT>(new WhereResultT());
            for (size_t or_idx=0; or_idx<inp.wterms; or_idx++) {
                shared_ptr<WhereResultT> ss;
                res.welem.push_back(ss);
            }
 
            Input& cinp = const_cast<Input&>(inp);
            res.current_chunk = cinp.chunk_q.fetch_and_increment();
            const uint32_t chunknum = res.current_chunk;
            if (chunknum < inp.chunk_size.size()) {

                string key = "QUERY:" + res.inp.qp.qid;

                // Update query status
                RedisAsyncConnection * rac = conns_[res.inp.redis_host_idx][res.inp.cnum].get();
                string rkey = "REPLY:" + res.inp.qp.qid;
                char stat[40];
                uint prg = 10 + (chunknum * 75)/inp.chunk_size.size();
                sprintf(stat,"{\"progress\":%d}", prg);
                RedisAsyncArgCommand(rac, NULL, 
                    list_of(string("RPUSH"))(rkey)(stat));

               return boost::bind(&QueryEngine::QueryExecWhere, qosp_->qe_,
                      _1, inp.qp, chunknum, 0);
            } else {
                return NULL;
            }
        }

        res.ret_info.push_back(exts[step-1]->perf);
        if (exts[step-1]->perf.error) {
            res.ret_code =false;
        }
        if (!res.ret_code) return NULL;

        // Number of substeps per chunk is the number of OR terms in WHERE
        // plus one more substep for select and post processing
        uint32_t substep = step % (inp.wterms + 1);

        if (substep == inp.wterms) {
            // Get the result of the final WHERE
            res.welem[substep-1] = exts[step-1]->wres;

            // The set "OR" API needs raw pointers
            vector<WhereResultT*> oterms;
            for (size_t or_idx = 0; or_idx < inp.wterms; or_idx++) {
                oterms.push_back(res.welem[or_idx].get());
            }

            // Do SET operations
            QE_ASSERT(res.wresult->size() == 0);
            SetOperationUnit::op_or(res.inp.qp.qid, *res.wresult, oterms);

	    for (size_t or_idx=0; or_idx<inp.wterms; or_idx++) {
                res.welem[or_idx].reset();
            }

            // Start the SELECT and POST-processing
            return boost::bind(&QueryEngine::QueryExec, qosp_->qe_,
                   _1, inp.qp, res.current_chunk, res.wresult.get());

        } else if (substep == 0) {
            // A chunk is complete. Start another one
            res.wresult->clear();
            uint32_t added_rows;

            if (inp.need_merge) {
                uint64_t then = UTCTimestampUsec();
                if (inp.map_output) {
                    uint32_t base_rows = res.mresult->size();
                    // TODO: This interface should not be Stats-Specific
                    StatsSelect::Merge(std::string(""), *(exts[step-1]->mres), *(res.mresult));
                    // Some rows of this chunk will merge into existing results
                    added_rows = res.mresult->size() - base_rows;
                } else {
                    uint32_t base_rows = res.result->size();
                    res.ret_code =
                        qosp_->qe_->QueryAccumulate(inp.qp,
                            *(exts[step-1]->res), *(res.result));
                    // Some rows of this chunk will merge into existing results
                    added_rows = res.result->size() - base_rows;
                }
                res.chunk_merge_time.push_back(
                    static_cast<uint32_t>((UTCTimestampUsec() - then)/1000));
        
            } else {
                // TODO : When merge is not needed, we can just send
                //        a result upto redis at this point.

                if (inp.map_output) {
                    added_rows = exts[step-1]->mres->size();
                    OutRowMultimapT::iterator jt = res.mresult->begin();
                    for (OutRowMultimapT::const_iterator it = exts[step-1]->mres->begin();
                            it != exts[step-1]->mres->end(); it++ ) {

                        jt = res.mresult->insert(jt,
                                std::make_pair(it->first, it->second));
                    }
                } else {
                    added_rows = exts[step-1]->res->size();
                    res.result->insert(res.result->begin(),
                        exts[step-1]->res->begin(),
                        exts[step-1]->res->end());                        
                }
            }
            Input& cinp = const_cast<Input&>(inp);
            if (cinp.total_rows.fetch_and_add(added_rows) > cinp.max_rows) {
                QE_LOG_NOQID(ERROR,  "QueryExec Max Rows Exceeded " <<
                    cinp.total_rows << " chunk " << cinp.chunk_q);
                return NULL;
            }
            
	    res.current_chunk = cinp.chunk_q.fetch_and_increment();
            const uint32_t chunknum = res.current_chunk;
            if (chunknum < inp.chunk_size.size()) {
                string key = "QUERY:" + res.inp.qp.qid;

                // Update query status
                RedisAsyncConnection * rac = conns_[res.inp.redis_host_idx][res.inp.cnum].get();
                string rkey = "REPLY:" + res.inp.qp.qid;
                char stat[40];
                uint prg = 10 + (chunknum * 75)/inp.chunk_size.size();
                sprintf(stat,"{\"progress\":%d}", prg);
                RedisAsyncArgCommand(rac, NULL, 
                    list_of(string("RPUSH"))(rkey)(stat));         
                return boost::bind(&QueryEngine::QueryExecWhere, qosp_->qe_,
                        _1, inp.qp, chunknum, 0);
            } else {
                return NULL;
            }            
        } else {
            // We are in the middle of doing WHERE processing for a chunk

            res.welem[substep-1] = exts[step-1]->wres;

            return boost::bind(&QueryEngine::QueryExecWhere, qosp_->qe_,
                    _1, inp.qp, res.current_chunk, substep);
        }
        return NULL;
    }

    struct Stage0Merge {
        Input inp;
        bool ret_code;
        bool overflow;
        uint32_t fm_time;
        vector<vector<QPerfInfo> > ret_info;
        vector<vector<uint32_t> > chunk_merge_time;
        BufferT result;
        OutRowMultimapT mresult;
    };
    bool QueryMerge(const std::vector<boost::shared_ptr<Stage0Out> > & subs,
           const boost::shared_ptr<Input> & inp, Stage0Merge & res) {

        res.ret_code = true;
        res.overflow = false;
        res.inp = subs[0]->inp;
        res.fm_time = 0;

        uint32_t total_rows = 0;      
        for (vector<shared_ptr<Stage0Out> >::const_iterator it = subs.begin() ;
                it!=subs.end(); it++) {
            if (res.inp.map_output)
                total_rows += (*it)->mresult->size();
            else
                total_rows += (*it)->result->size();
        }

        // If max_rows have been exceeded, don't do any more processing
        if (total_rows > res.inp.max_rows) {
            res.overflow = true;
            return true;
        }

        std::vector<boost::shared_ptr<OutRowMultimapT> > mqsubs;
        std::vector<boost::shared_ptr<QEOpServerProxy::BufferT> > qsubs;
        for (vector<shared_ptr<Stage0Out> >::const_iterator it = subs.begin() ;
                it!=subs.end(); it++) {

            res.ret_info.push_back((*it)->ret_info);
            res.chunk_merge_time.push_back((*it)->chunk_merge_time);

            if ((*it)->ret_code == false) {
                res.ret_code = false;
            } else {
                if (res.inp.map_output)
                    mqsubs.push_back((*it)->mresult);
                else
                    qsubs.push_back((*it)->result);
            }
        }

        if (!res.ret_code) return true;

        if (res.inp.need_merge) {
            uint64_t then = UTCTimestampUsec();

            if (res.inp.map_output) {
                res.ret_code =
                    qosp_->qe_->QueryFinalMerge(res.inp.qp, mqsubs, res.mresult);

            } else {
                res.ret_code =
                    qosp_->qe_->QueryFinalMerge(res.inp.qp, qsubs, res.result);
            }

            uint64_t now = UTCTimestampUsec();
            res.fm_time = static_cast<uint32_t>((now - then)/1000);
        } else {
            // TODO : If a merge was not needed, results have been sent to 
            //        redis already. The only thing still needed is the status
            for (vector<shared_ptr<Stage0Out> >::const_iterator it = subs.begin() ;
                    it!=subs.end(); it++) {

                if (res.inp.map_output) {
                    OutRowMultimapT::iterator jt = res.mresult.begin();
                    for (OutRowMultimapT::const_iterator kt = (*it)->mresult->begin();
                            kt != (*it)->mresult->end(); kt++) {
                        jt = res.mresult.insert(jt,
                                std::make_pair(kt->first, kt->second));
                    }
                } else {
                    res.result.insert(res.result.begin(),
                        (*it)->result->begin(),
                        (*it)->result->end());
                }
            }
        }

        return true;
    }

    struct Output {
        Input inp;
        uint32_t redis_time;
        bool ret_code;
    };
    ExternalBase::Efn QueryResp(uint32_t inst, const vector<RedisT*> & exts,
            const Stage0Merge & inp, Output & ret) {
        uint32_t step = exts.size();
        switch (inst) {
        case 0: {
                if (!step)  {

                    ret.inp = inp.inp;
                    RedisAsyncConnection * rac = conns_[ret.inp.redis_host_idx][ret.inp.cnum].get();
                    std::stringstream keystr;
                    auto_ptr<QEOutputT> jsonresult(new QEOutputT);

                    QE_LOG_NOQID(INFO,  "Will Jsonify #rows " << 
                        inp.result.size() + inp.mresult.size());
                    QueryJsonify(inp.inp.table, inp.inp.map_output,
                        &inp.result, &inp.mresult, jsonresult.get());
                        
                    vector<string> const * const res = jsonresult.get();
                    vector<string>::size_type idx = 0;
                    uint32_t rownum = 0;

                    QE_LOG_NOQID(INFO,  "Did Jsonify #rows " << res->size());
                    
                    uint64_t then = UTCTimestampUsec();
                    char stat[80];
                    string key = "REPLY:" + ret.inp.qp.qid;
                    
                    if (inp.overflow) {
                        sprintf(stat,"{\"progress\":%d}", - ENOBUFS);
                    } else if (!inp.ret_code) {
                        sprintf(stat,"{\"progress\":%d}", - EIO);
                    } else {
                        while (idx < res->size()) {
                            uint32_t rowsize = 0;
                            keystr.str(string());
                            keystr << "RESULT:" << ret.inp.qp.qid << ":" << rownum;
                            vector<string> command = list_of(string("RPUSH"))(keystr.str());
                            while ((idx < res->size()) && (((int)rowsize) < kMaxRowThreshold)) {
                                command.push_back(res->at(idx));
                                rowsize += res->at(idx).size();
                                idx++;
                            }
                            RedisAsyncArgCommand(rac, NULL, command);
                            RedisAsyncArgCommand(rac, NULL, 
                                list_of(string("EXPIRE"))(keystr.str())("300"));
                            sprintf(stat,"{\"progress\":90, \"lines\":%d}",
                                (int)rownum);
                            RedisAsyncArgCommand(rac, NULL, 
                                list_of(string("RPUSH"))(key)(stat));
                            rownum++;
                        }
                        sprintf(stat,"{\"progress\":100, \"lines\":%d, \"count\":%d}",
                            (int)rownum, (int)res->size());
                    }
                    uint64_t now = UTCTimestampUsec();
                    ret.redis_time = static_cast<uint32_t>((now - then)/1000);
                    QE_LOG_NOQID(DEBUG,  "QE Query Result is " << stat);
                    return boost::bind(&RedisAsyncArgCommand,
                            conns_[ret.inp.redis_host_idx][ret.inp.cnum].get(), _1,
                            list_of(string("RPUSH"))(key)(stat));   
                } else {
                    RedisAsyncConnection * rac = conns_[ret.inp.redis_host_idx][ret.inp.cnum].get();
                    string key = "REPLY:" + ret.inp.qp.qid;
                    RedisAsyncArgCommand(rac, NULL,
                        list_of(string("EXPIRE"))(key)("300"));

                    key = "QUERY:" + ret.inp.qp.qid;
                    RedisAsyncArgCommand(rac, NULL,
                        list_of(string("EXPIRE"))(key)("300"));

                    uint64_t now = UTCTimestampUsec();
                    uint32_t qtime = static_cast<uint32_t>(
                            (now - ret.inp.qp.query_starttm)/1000);

                    uint64_t enqtm = atol(ret.inp.qp.terms["enqueue_time"].c_str());
                    uint32_t enq_delay = static_cast<uint32_t>(
                            (ret.inp.qp.query_starttm - enqtm)/1000);

                    QueryStats qs;
                    size_t outsize;

                    if (ret.inp.map_output)
                        outsize = inp.mresult.size();
                    else
                        outsize = inp.result.size();

                    qs.set_rows(static_cast<uint32_t>(outsize));                                           
                    qs.set_time(qtime);
                    qs.set_qid(ret.inp.qp.qid);
                    qs.set_chunks(inp.inp.chunk_size.size());
                    std::ostringstream wherestr, selstr, poststr;
                    for (size_t i=0; i < inp.ret_info.size(); i++) {
                        for (size_t j=0; j < inp.ret_info[i].size(); j++) {
                            wherestr << inp.ret_info[i][j].chunk_where_time << ",";
                            selstr << inp.ret_info[i][j].chunk_select_time << ",";
                            poststr << inp.ret_info[i][j].chunk_postproc_time << ",";
                        }
                        wherestr << " ";
                        selstr << " ";
                        poststr << " ";
                    }
                    if (inp.overflow) {
                        qs.set_error("ERROR-ENOBUFS");
                    } else if (!inp.ret_code) {
                        qs.set_error("ERROR-EIO");
                    } else {
                        qs.set_error("None");
                    }
                    qs.set_chunk_where_time(wherestr.str());
                    qs.set_chunk_select_time(selstr.str());
                    qs.set_chunk_postproc_time(poststr.str());

                    std::ostringstream mergestr;
                    for (size_t i=0; i < inp.chunk_merge_time.size(); i++) {
                        for (size_t j=0; j < inp.chunk_merge_time[i].size(); j++) {
                            mergestr << inp.chunk_merge_time[i][j] << ",";
                        }
                        mergestr << " ";
                    }

                    qs.set_chunk_merge_time(mergestr.str());
                    qs.set_final_merge_time(inp.fm_time);
                    qs.set_where(inp.inp.where);
                    qs.set_select(inp.inp.select);
                    qs.set_post(inp.inp.post);
                    qs.set_time_span(static_cast<uint32_t>(inp.inp.time_period));
                    qs.set_enq_delay(enq_delay);
                    QUERY_PERF_INFO_SEND(Sandesh::source(), // name
                                         inp.inp.table,     // table
                                         qs);

                    QE_LOG_NOQID(INFO, "Finished: QID " << ret.inp.qp.qid <<
                        " Table " << inp.inp.table <<
                        " Time(ms) " << qtime <<
                        " RedisTime(ms) " << ret.redis_time <<
                        " MergeTime(ms) " << inp.fm_time <<
                        " Rows " << outsize <<
                        " EnQ-delay" << enq_delay);

                    ret.ret_code = true;
                }
            }
            break;
        case 1: {
                if (!step)  {
                    string key = "ENGINE:" + inp.inp.hostname;
                    return boost::bind(&RedisAsyncArgCommand,
                            conns_[inp.inp.redis_host_idx][inp.inp.cnum].get(), _1,
                            list_of(string("LREM"))(key)("0")(inp.inp.qp.qid));
                } else {
                    ret.ret_code = true;
                }
            }
            break;
        }
        return NULL;
    }


    typedef WorkPipeline<Input, Stage0Merge, Output> QEPipeT;
               
    void QEPipeCb(QEPipeT *wp, bool ret_code) {
        tbb::mutex::scoped_lock lock(mutex_);

        boost::shared_ptr<Output> res = wp->Result();
        assert(pipes_.find(res->inp.qp.qid)->second == wp);
        pipes_.erase(res->inp.qp.qid);
        m_analytics_queries.erase(res->inp.qp.qid);
        npipes_[res->inp.redis_host_idx][res->inp.cnum-1]--;
        QE_LOG_NOQID(DEBUG,  " Result " << res->ret_code << " , " << res->inp.cnum << " conn");
        delete wp;
    }

    void ConnUpPostProcess(int redis_host_idx, uint8_t cnum) {
        if (!connState_[redis_host_idx][cnum]) {
            QE_LOG_NOQID(DEBUG, "ConnUp SetCB" << (uint32_t)cnum);
            cb_proc_fn_[redis_host_idx][cnum] = boost::bind(&QEOpServerImpl::CallbackProcess,
                    this, redis_host_idx, cnum, _1, _2, _3);
            conns_[redis_host_idx][cnum].get()->SetClientAsyncCmdCb(cb_proc_fn_[redis_host_idx][cnum]);
            connState_[redis_host_idx][cnum] = true;
        }

        bool isConnected = true;
        for(int i=0; i<kConnections+1; i++)
            if (!connState_[redis_host_idx][i]) isConnected = false;
        if (isConnected) {
            string key = "ENGINE:" + hostname_;
            conns_[redis_host_idx][0].get()->RedisAsyncArgCmd(0,
                    list_of(string("BRPOPLPUSH"))("QUERYQ")(key)("0"));            
        }
    }

    int LeastLoadedConnection(int redis_host_idx) {
        int minvalue = npipes_[redis_host_idx][0];
        int minindex = 0;
        for (int i = 1; i < kConnections; i++) {
            if (npipes_[redis_host_idx][i] < minvalue) {
                minvalue = npipes_[redis_host_idx][i];
                minindex = i;
            }
        }
        return minindex;
    }

    void QueryError(int redis_host_idx, string qid, int ret_code) {
        string redis_host = redis_host_port_pairs_[redis_host_idx].first;
        int redis_port = redis_host_port_pairs_[redis_host_idx].second;
        redisContext *c = redisConnect(redis_host.c_str(), redis_port);
        if (c->err) {
            QE_LOG_NOQID(ERROR, "Cannot report query error for " << qid <<
                " . No Redis Connection");
            redisFree(c);
            return;
        }

        /* Secure the connection if SSL enabled */
        if (redis_ssl_enable_) {
            int rc = redisSecureConnection(c, redis_ca_cert_.c_str(),
                      redis_certfile_.c_str(), redis_keyfile_.c_str(), "sni");
            if (rc != REDIS_OK) {
                QE_LOG_NOQID(ERROR, "Cannot report query error for " << qid <<
                             " SSL Connection Error: " << c->errstr);
                redisFree(c);
                return;
            }
        }

        //Authenticate the context with password
        if (!redis_password_.empty()) {
            redisReply * reply = (redisReply *) redisCommand(c, "AUTH %s",
                                                             redis_password_.c_str());
            if (reply->type == REDIS_REPLY_ERROR) {
                QE_LOG_NOQID(ERROR, "Authentication to redis error");
                freeReplyObject(reply);
                redisFree(c);
                return;
            }
            freeReplyObject(reply);
        }

        char stat[80];
        string key = "REPLY:" + qid;
        sprintf(stat,"{\"progress\":%d}", - ret_code);

        redisReply * reply = (redisReply *) redisCommand(c, "RPUSH %s %s",
            key.c_str(), stat);
        
        freeReplyObject(reply);
        redisFree(c);
    }


    void StartPipeline(const string qid, int redis_host_idx) {
        QueryStats qs;
        qs.set_qid(qid);
        qs.set_rows(0);
        qs.set_time(0);
        qs.set_final_merge_time(0);
        qs.set_enq_delay(0);

        uint64_t now = UTCTimestampUsec();

        tbb::mutex::scoped_lock lock(mutex_);

        string redis_host = redis_host_port_pairs_[redis_host_idx].first;
        int redis_port = redis_host_port_pairs_[redis_host_idx].second;
        QE_LOG_NOQID(ERROR, "StartPipeline on :" << " Redis:" << redis_host << " Port:" << redis_port);
        redisContext *c = redisConnect(redis_host.c_str(), redis_port);

        if (c->err) {
            QE_LOG_NOQID(ERROR, "Cannot start Pipleline for " << qid <<
                " . No Redis Connection");
            redisFree(c);
            qs.set_error("No Redis Connection");
            QUERY_PERF_INFO_SEND(Sandesh::source(), // name
                                 "__UNKNOWN__",     // table
                                 qs);
            return;
        }

        /* Secure the connection if SSL enabled */
        if (redis_ssl_enable_) {
            int rc = redisSecureConnection(c, redis_ca_cert_.c_str(),
                       redis_certfile_.c_str(), redis_keyfile_.c_str(), "sni");
            if (rc != REDIS_OK) {
                QE_LOG_NOQID(ERROR, "Cannot start pipeline for " << qid <<
                                " SSL Connection Error: " << c->errstr);
                redisFree(c);
                qs.set_error("Redis SSL Connection Error");
                QUERY_PERF_INFO_SEND(Sandesh::source(), "__UNKNOWN__", qs);
                return;
            }
        }

        //Authenticate the context with password
        if ( !redis_password_.empty()) {
            redisReply * reply = (redisReply *) redisCommand(c, "AUTH %s",
                                                             redis_password_.c_str());
            if (reply->type == REDIS_REPLY_ERROR) {
                QE_LOG_NOQID(ERROR, "Authentication to redis error");
                freeReplyObject(reply);
                redisFree(c);
                qs.set_error("Redis Auth Failed");
                QUERY_PERF_INFO_SEND(Sandesh::source(), // name
                                     "__UNKNOWN__",     // table
                                     qs);
                return;
            }
            freeReplyObject(reply);
        }

        string key = "QUERY:" + qid;
        redisReply * reply = (redisReply *) redisCommand(c, "hgetall %s",
            key.c_str());
       
        map<string,string> terms; 
        if (!(c->err) && (reply->type == REDIS_REPLY_ARRAY)) {
            for (uint32_t i=0; i<reply->elements; i+=2) {
                string idx(reply->element[i]->str);
                string val(reply->element[i+1]->str);
                terms[idx] = val;
            }
        } else {
            QE_LOG_NOQID(ERROR, "Cannot start Pipleline for " << qid << 
                ". Could not read query input");
            freeReplyObject(reply);
            redisFree(c);
            QueryError(redis_host_idx, qid, 5);
            qs.set_error("Could not read query input");
            QUERY_PERF_INFO_SEND(Sandesh::source(), // name
                                 "__UNKNOWN__",     // table
                                 qs);
            return;
        }

        freeReplyObject(reply);
        redisFree(c);

        QueryEngine::QueryParams qp(qid, terms, max_tasks_,
            UTCTimestampUsec());
       
        vector<uint64_t> chunk_size;
        bool need_merge;
        bool map_output;
        string table;
        string where;
        uint32_t wterms;
        string select;
        string post;
        uint64_t time_period;

        int ret = qosp_->qe_->QueryPrepare(qp, chunk_size, need_merge, map_output,
            where, wterms, select, post, time_period, table);

        qs.set_where(where);
        qs.set_select(select);
        qs.set_post(post);
        qs.set_time_span(time_period);
        uint64_t enqtm = atol(terms["enqueue_time"].c_str());
        uint32_t enq_delay = static_cast<uint32_t>((now - enqtm)/1000);
        qs.set_enq_delay(enq_delay);

        if (ret!=0) {
            QueryError(redis_host_idx, qid, ret);
            QE_LOG_NOQID(ERROR, "Cannot start Pipleline for " << qid << 
                ". Query Parsing Error " << ret);
            qs.set_error("Query Parsing Error");
            QUERY_PERF_INFO_SEND(Sandesh::source(), // name
                                 table,             // table
                                 qs);
            return;
        } else {
            QE_LOG_NOQID(INFO, "Chunks: " << chunk_size.size() <<
                " Need Merge: " << need_merge);
        }

        if (pipes_.size() >= 32) {
            QueryError(redis_host_idx, qid, EMFILE);
            QE_LOG_NOQID(ERROR, "Cannot start Pipleline for " << qid <<
                ". Too many queries : " << pipes_.size());
            qs.set_error("EMFILE");
            QUERY_PERF_INFO_SEND(Sandesh::source(), // name
                                 table,             // table
                                 qs);
            return;
        }

        shared_ptr<Input> inp(new Input());
        inp.get()->hostname = hostname_;
        inp.get()->qp = qp;
        inp.get()->map_output = map_output;
        inp.get()->need_merge = need_merge;
        inp.get()->chunk_size = chunk_size;
        inp.get()->where = where;
        inp.get()->select = select;
        inp.get()->post = post;
        inp.get()->time_period = time_period;
        inp.get()->table = table;
        inp.get()->chunk_q = 0;
        inp.get()->total_rows = 0;
        inp.get()->max_rows = max_rows_;
        inp.get()->wterms = wterms;
        vector<pair<int,int> > tinfo;
        for (uint idx=0; idx<(uint)max_tasks_; idx++) {
            tinfo.push_back(make_pair(0, -1));
        }

        QEPipeT  * wp = new QEPipeT(
            new WorkStage<Input, Stage0Merge,
                    RawResultT, Stage0Out>(
                tinfo,
                boost::bind(&QEOpServerImpl::QueryExec, this, _1,_2,_3,_4),
                boost::bind(&QEOpServerImpl::QueryMerge, this, _1,_2,_3)),
            new WorkStage<Stage0Merge, Output,
                    RedisT>(
                list_of(make_pair(0,-1))(make_pair(0,-1)),
                boost::bind(&QEOpServerImpl::QueryResp, this, _1,_2,_3,_4)));

        pipes_.insert(make_pair(qid, wp));

        // Initialize the m_analytics_queries for this qid
        m_analytics_queries[qid] = std::vector<boost::shared_ptr<AnalyticsQuery> > ();

        int conn = LeastLoadedConnection(redis_host_idx);
        QE_LOG_NOQID(DEBUG, "Getting Least Loaded Conn as :" << conn);
        npipes_[redis_host_idx][conn]++;
        
        // The cnum with index 0 is only used for receiving new queries
        inp.get()->cnum = conn+1; 
        inp.get()->redis_host_idx = redis_host_idx;

        wp->Start(boost::bind(&QEOpServerImpl::QEPipeCb, this, wp, _1), inp);
        QE_LOG_NOQID(DEBUG, "Starting Pipeline for " << qid << " , " << conn+1 << 
            " conn, " << tinfo.size() << " tasks");
        
        // Update query status
        RedisAsyncConnection * rac = conns_[redis_host_idx][inp.get()->cnum].get();
        string rkey = "REPLY:" + qid;
        char stat[40];
        sprintf(stat,"{\"progress\":15}");
        RedisAsyncArgCommand(rac, NULL, 
            list_of(string("RPUSH"))(rkey)(stat));


    }

    void ConnUpPrePostProcess(int redis_host_idx, uint8_t cnum) {
        //Assign callback for AUTH command
        cb_proc_fn_[redis_host_idx][cnum] = boost::bind(&QEOpServerImpl::ConnectCallbackProcess,
                    this, redis_host_idx, cnum, _1, _2, _3);
        conns_[redis_host_idx][cnum].get()->SetClientAsyncCmdCb(cb_proc_fn_[redis_host_idx][cnum]);
        //Send AUTH command
        RedisAsyncConnection * rac = conns_[redis_host_idx][cnum].get();
        if (!redis_password_.empty()) {
            RedisAsyncArgCommand(rac, NULL,
                    list_of(string("AUTH"))(redis_password_.c_str()));
        } else {
            RedisAsyncArgCommand(rac, NULL,
                    list_of(string("PING")));
        }
    }

    void ConnUp(int redis_host_idx, uint8_t cnum) {
        string redis_host = redis_host_port_pairs_[redis_host_idx].first;
        std::ostringstream ostr;
        ostr << "ConnUp.. UP " << (uint32_t)cnum << " With Redis:" << redis_host;
        QE_LOG_NOQID(DEBUG, ostr.str());
        qosp_->evm_->io_service()->post(
                    boost::bind(&QEOpServerImpl::ConnUpPrePostProcess,
                    this, redis_host_idx, cnum));
    }

    void UpdateRedisConnectionStatus () {
        std::ostringstream ostr;
        map<string, string>::iterator it;
        vector<string> redis_endpoints;
        int redis_hosts_cnt = redis_host_port_pairs_.size();
        int redis_down_count = 0;
        int redis_idx = 0;
        int conn_idx = 0;
        vector<Endpoint> redis_endpoint_list;
        for (redis_idx = 0; redis_idx < redis_hosts_cnt; redis_idx++) {
            string redis_host(redis_host_port_pairs_[redis_idx].first);
            redis_endpoint_list.push_back(conns_[redis_idx][0]->Endpoint());
            for (conn_idx = 0; conn_idx < kConnections + 1; conn_idx++) {
                if (true == connState_[redis_idx][conn_idx]) {
                    break;
                }
            }
            if (conn_idx == kConnections + 1) {
                ostr << conns_[redis_idx][0]->Endpoint() << "::Down.";
                redis_down_count++;
            }
        }
        if (redis_down_count < redis_hosts_cnt) {
            /* Then QE should be UP, but display if any redis connection is down */
            ConnectionState::GetInstance()->Update(ConnectionType::REDIS_QUERY,
                "Query", ConnectionStatus::UP, redis_endpoint_list, ostr.str());
        } else {
            std::ostringstream empty_ostr;
            ConnectionState::GetInstance()->Update(ConnectionType::REDIS_QUERY,
                "Query", ConnectionStatus::DOWN, redis_endpoint_list, empty_ostr.str());
        }
    }

    void ConnDown(int redis_host_idx, uint8_t cnum) {
        string redis_host = redis_host_port_pairs_[redis_host_idx].first;
        std::ostringstream ostr;
        ostr << "ConnDown.. DOWN.. Reconnect.." << (uint32_t)cnum << " With Redis:" << redis_host;
        QE_LOG_NOQID(DEBUG, ostr.str());
        connState_[redis_host_idx][cnum] = false;
        qosp_->evm_->io_service()->post(
                     boost::bind(&QEOpServerImpl::UpdateRedisConnectionStatus, this));
        qosp_->evm_->io_service()->post(boost::bind(&RedisAsyncConnection::RAC_Connect,
            conns_[redis_host_idx][cnum].get()));
    }

    void ConnectCallbackProcess(int redis_host_idx, uint8_t cnum, const redisAsyncContext *c, void *r, void *privdata) {
        QE_LOG_NOQID(DEBUG, "UP ConnectCallbackProcess.." << conns_[redis_host_idx][cnum]->Endpoint());
        if (r == NULL) {
            QE_LOG_NOQID(DEBUG, "In ConnectCallbackProcess.. NULL Reply");
            return;
        }
        redisReply reply = *reinterpret_cast<redisReply*>(r);
        if (reply.type != REDIS_REPLY_ERROR) {
            QE_LOG_NOQID(DEBUG, "In ConnectCallbackProcess.." << conns_[redis_host_idx][cnum]->Endpoint());
            qosp_->evm_->io_service()->post(
                     boost::bind(&QEOpServerImpl::UpdateRedisConnectionStatus, this));
            qosp_->evm_->io_service()->post(
                     boost::bind(&QEOpServerImpl::ConnUpPostProcess,
                     this, redis_host_idx, cnum));
        } else {
            QE_LOG_NOQID(ERROR,"In connectCallbackProcess.. Error");
            QE_ASSERT(reply.type != REDIS_REPLY_ERROR);
        }
    }

    void CallbackProcess(int redis_host_idx, uint8_t cnum, const redisAsyncContext *c, void *r, void *privdata) {

        //QE_TRACE_NOQID(DEBUG, "Redis CB" << cnum);
        if (0 == cnum) {
            if (r == NULL) {
                QE_LOG_NOQID(DEBUG,  __func__ << ": received NULL reply from redis");
                return;
            }

            redisReply reply = *reinterpret_cast<redisReply*>(r);
            if (reply.type != REDIS_REPLY_STRING) {
                QE_LOG_NOQID(ERROR,  __func__ << " Bad Redis reply on control connection: " << reply.type);
                if (reply.type == REDIS_REPLY_ERROR) {
                    string errstr(reply.str);
                    QE_LOG_NOQID(ERROR,  __func__ << " Redis Error: " << reply.str);
                    sleep(1000);
                }
            }
            QE_ASSERT(reply.type == REDIS_REPLY_STRING);
            string qid(reply.str);

            StartPipeline(qid, redis_host_idx);

            qosp_->evm_->io_service()->post(
                    boost::bind(&QEOpServerImpl::ConnUpPostProcess,
                    this, redis_host_idx, cnum));
            return;
        }

        auto_ptr<RedisT> fullReply;
        vector<string> elements;

        if (r == NULL) {
            //QE_TRACE_NOQID(DEBUG, "NULL Reply...\n");
        } else {
            redisReply reply = *reinterpret_cast<redisReply*>(r);
            fullReply.reset(new RedisT);
            fullReply.get()->first = reply;
            if (reply.type == REDIS_REPLY_ARRAY) {
                for (uint32_t i=0; i<reply.elements; i++) {
                    string element(reply.element[i]->str);
                    fullReply.get()->second.push_back(element);
                }
            } else if (reply.type == REDIS_REPLY_STRING) {
                fullReply.get()->second.push_back(string(reply.str));
            }
        }
        if (!privdata) {
            //QE_TRACE_NOQID(DEBUG, "Ignoring redis reply");
            return;
        }      
        ExternalProcIf<RedisT> * rpi = 
                reinterpret_cast<ExternalProcIf<RedisT> *>(privdata);
        QE_TRACE_NOQID(DEBUG,  " Rx data from REDIS for " << rpi->Key());
                    
        rpi->Response(fullReply);

    }

    void BuildRedisIPPort(vector<string> redis_ip_port_list) {
        typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
        for (vector<string>::const_iterator it = redis_ip_port_list.begin();
             it != redis_ip_port_list.end(); it++) {
            string redis_ip_port(*it);
            boost::char_separator<char> sep(":");
            tokenizer tokens(redis_ip_port, sep);
            tokenizer::iterator tit = tokens.begin();
            string redis_ip(*tit);
            ++tit;
            string redis_port(*tit);
            redis_host_port_pairs_.push_back(make_pair(redis_ip, atoi(redis_port.c_str())));
        }
    }

    QEOpServerImpl(vector<string> redis_ip_ports,
                   const string & redis_password,
                   const bool redis_ssl_enable,
                   const string & redis_keyfile,
                   const string & redis_certfile,
                   const string & redis_ca_cert,
                   QEOpServerProxy * qosp, int max_tasks,
                   int max_rows, const string &host_ip) :
            hostname_(ResolveCanonicalName(host_ip)),
            redis_password_(redis_password),
            redis_ssl_enable_(redis_ssl_enable),
            redis_keyfile_(redis_keyfile),
            redis_certfile_(redis_certfile),
            redis_ca_cert_(redis_ca_cert),
            qosp_(qosp),
            max_tasks_(max_tasks),
            max_rows_(max_rows) {
        int redis_host_idx = 0;
        /* Initialize */
        BuildRedisIPPort(redis_ip_ports);
        int redis_host_count = redis_host_port_pairs_.size();
        for (int i = 0; i < redis_host_count; i++) {
            cb_proc_fn_[i] = new RedisAsyncConnection::ClientAsyncCmdCbFn[kConnections + 1];
            conns_[i] = new boost::shared_ptr<RedisAsyncConnection>[kConnections + 1];
            npipes_[i] = new int[kConnections];
            connState_[i] = new bool[kConnections + 1];
        }
        for (redis_host_idx = 0; redis_host_idx < int(redis_host_count); redis_host_idx++) {
            string redis_host = redis_host_port_pairs_[redis_host_idx].first;
            int redis_port = redis_host_port_pairs_[redis_host_idx].second;
            for (int i = 0; i < kConnections + 1; i++) {
                cb_proc_fn_[redis_host_idx][i] = boost::bind(&QEOpServerImpl::CallbackProcess,
                                             this, redis_host_idx, i, _1, _2, _3);
                connState_[redis_host_idx][i] = false;
                if (i) {
                    conns_[redis_host_idx][i].reset(rac_alloc(qosp->evm_, redis_host, redis_port,
                                  boost::bind(&QEOpServerImpl::ConnUp, this, redis_host_idx, i),
                                  boost::bind(&QEOpServerImpl::ConnDown, this, redis_host_idx, i),
                                  redis_ssl_enable, redis_keyfile, redis_certfile, redis_ca_cert));
                } else {
                    conns_[redis_host_idx][i].reset(rac_alloc_nocheck(qosp->evm_, redis_host, redis_port,
                                  boost::bind(&QEOpServerImpl::ConnUp, this, redis_host_idx, i),
                                  boost::bind(&QEOpServerImpl::ConnDown, this, redis_host_idx, i),
                                  redis_ssl_enable, redis_keyfile, redis_certfile, redis_ca_cert));
                }
                // The cnum with index 0 is only used for receiving new queries
                // It does not host any pipelines
                if (i) npipes_[redis_host_idx][i-1] = 0;
            }
        }
    }

    ~QEOpServerImpl() {
    }

    void AddAnalyticsQuery(const std::string &qid,
                           boost::shared_ptr<AnalyticsQuery> q) {
        tbb::mutex::scoped_lock lock(mutex_);
        m_analytics_queries[qid].push_back(q);
    }

private:

    static const int kMaxRowThreshold = 10000;

    // We always have one connection to receive new queries from OpServer
    // This is the number of addition connections, which will be 
    // used to read query parameters and write query results
    static const uint8_t kConnections = 4;

    const string hostname_;
    map<string, string> redis_status_maps;
    const string redis_password_;
    const bool redis_ssl_enable_;
    const string redis_keyfile_;
    const string redis_certfile_;
    const string redis_ca_cert_;
    QEOpServerProxy * const qosp_;
    boost::shared_ptr<RedisAsyncConnection> *conns_[kConnections+1];
    RedisAsyncConnection::ClientAsyncCmdCbFn *cb_proc_fn_[kConnections+1];
    bool *connState_[kConnections+1];

    tbb::mutex mutex_;
    map<string,QEPipeT*> pipes_;
    vector<pair<string, int> > redis_host_port_pairs_;
    int *npipes_[kConnections];
    int max_tasks_;
    int max_rows_;
    std::map<std::string, std::vector<boost::shared_ptr<AnalyticsQuery> > >
        m_analytics_queries;

};

QEOpServerProxy::QEOpServerProxy(EventManager *evm, QueryEngine *qe,
            vector<string> redis_ip_ports,
            const string & redis_password,
            const bool redis_ssl_enable,
            const string & redis_keyfile,
            const string & redis_certfile,
            const string & redis_ca_cert,
            const std::string &host_ip, int max_tasks, int max_rows) :
        evm_(evm),
        qe_(qe),
        impl_(new QEOpServerImpl(redis_ip_ports, redis_password, redis_ssl_enable,
            redis_keyfile, redis_certfile, redis_ca_cert,
            this, max_tasks, max_rows, host_ip)) {}

QEOpServerProxy::~QEOpServerProxy() {}

void
QEOpServerProxy::QueryResult(void * qid, QPerfInfo qperf,
        auto_ptr<BufferT> res, auto_ptr<OutRowMultimapT> mres) {
        
    impl_->QECallback(qid, qperf, res, mres);
}

void
QEOpServerProxy::QueryResult(void * qid, QPerfInfo qperf,
        auto_ptr<std::vector<query_result_unit_t> > res) {
        
    impl_->QECallback(qid, qperf, res);
}

void QEOpServerProxy::AddAnalyticsQuery(const std::string &qid,
        boost::shared_ptr<AnalyticsQuery> q) {
    impl_->AddAnalyticsQuery(qid, q);
}
