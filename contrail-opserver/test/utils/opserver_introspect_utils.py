#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

import sys
vizdtestdir = sys.path[0]

import urllib2
import xmltodict
import json
import requests
import socket
from lxml import etree
from opserver.introspect_util import *
from opserver_results import *
from opserver.opserver_util import OpServerUtils

class VerificationOpsSrvIntrospect (IntrospectUtilBase):
    def __init__(self, ip, port):
        super(VerificationOpsSrvIntrospect, self).__init__(ip, port, drv=XmlDrv)

    def db_info_set_request(self, disk_usage_percentage,
                            pending_compaction_tasks):
        disk_usage_percentage_str = ''
        if (disk_usage_percentage != None):
            disk_usage_percentage_str = str(disk_usage_percentage)
        pending_compaction_tasks_str = ''
        if (pending_compaction_tasks != None):
            pending_compaction_tasks_str = str(pending_compaction_tasks)

        path = 'Snh_DbInfoSetRequest?disk_usage_percentage=%s' \
               '&pending_compaction_tasks=%s' % \
               (disk_usage_percentage_str, pending_compaction_tasks_str)
        self.dict_get(path)

    def db_info_get_request(self):
        path = 'Snh_DbInfoGetRequest?'
        xpath = '/DbInfoResponse/db_info'
        p = self.dict_get(path)
        return EtreeToDict(xpath).get_all_entry(p)

    def get_redis_uve_info(self):
        path = 'Snh_RedisUVERequest'
        xpath = '/RedisUVEResponse/redis_uve_info'
        p = self.dict_get(path)
        return EtreeToDict(xpath).get_all_entry(p)

class VerificationOpsSrv (IntrospectUtilBase):
    def __init__(self, ip, port=8181, user='test',
                 password='password',
                 headers = {'X-Auth-Token' : 'user:admin'}, sandesh_config=None):
        super(VerificationOpsSrv, self).__init__(ip, port, drv=JsonDrv, config=sandesh_config)
        self._user = user
        self._password = password
        self._headers = headers

    def get_ops_vm(self, vm='default-virtual-machine'):
        vm_dict = self.dict_get('analytics/uves/virtual-machine/' + vm,
            user=self._user, password=self._password, headers=self._headers)
        return OpVMResult(vm_dict)

    def get_ops_vn(self, vn='default-virtual-network'):
        res = None
        try:
            vn_dict = self.dict_get('analytics/uves/virtual-network/' + vn,
                user=self._user, password=self._password,
                headers=self._headers)
            res = OpVNResult(vn_dict)
        except Exception as e:
            print e
        finally:
            return res

    def get_ops_vns(self):
        res = dict()
        try:
            res = self.dict_get('analytics/uves/virtual-networks',
                user=self._user, password=self._password,
                headers=self._headers)
        except Exception as e:
            print e
        finally:
            return res

    def get_ops_vns_response(self):
        vns_url = self._http_str + \
            "://%s:%d/analytics/uves/virtual-networks" % (self._ip, self._port)
        headers = {'X-Auth-Token':'admin:password'}
        res = requests.get(vns_url, headers=headers, timeout=10)
        return res

    def get_ops_collector(self, col=None):
        if (col is None):
            col = socket.getfqdn("127.0.0.1")
        res = None
        try:
            col_dict = self.dict_get('analytics/uves/analytics-node/' + col,
                user=self._user, password=self._password,
                headers=self._headers)
            res = OpCollectorResult(col_dict)
        except Exception as e:
            print e
        finally:
            return res

    def send_tracebuffer_req(self, src, mod, instance, buf_name):
        return self.dict_get('analytics/send-tracebuffer/%s/%s/%s/%s' \
                             % (src, mod, instance, buf_name), user=self._user,
                             password=self._password, headers=self._headers)

    def get_table_column_values(self, table, col_name):
        return self.dict_get('analytics/table/%s/column-values/%s' \
                             % (table, col_name), user=self._user,
                             password=self._password, headers=self._headers)

    def uve_query(self, table, query):
        return self.dict_get('analytics/uves/%s' % (table), query,
            user=self._user, password=self._password, headers=self._headers)

    def get_alarms(self, filters):
        return self.dict_get('analytics/alarms', filters, user=self._user,
            password=self._password, headers=self._headers)

    def post_uve_request(self, table, json_body):
        url = 'http://%s:%s/analytics/uves/%s' % (self._ip, str(self._port), table)
        try:
            res = OpServerUtils.post_url_http(url, json_body, user=self._user,
                password=self._password, sync=True, headers=self._headers)
            res = json.loads(res)
        except Exception as e:
            print 'Error: POST uve request: %s' % str(e)
            return None
        else:
            return res
    # end post_uve_request

    def post_query_json(self, json_str, sync=True):
        '''
        this module is to support raw query given in json format
        '''
        res = None
        try:
            flows_url = OpServerUtils.opserver_query_url(self._ip,
                str(self._port))
            print flows_url
            print "query is: ", json_str
            res = []
            resp = OpServerUtils.post_url_http(flows_url, json_str,
                self._user, self._password, sync, headers=self._headers)
            if sync:
                if resp is not None:
                    res = json.loads(resp)
                    res = res['value']
            else: 
                if resp is not None:
                    resp = json.loads(resp)
                    qid = resp['href'].rsplit('/', 1)[1]
                    result = OpServerUtils.get_query_result(self._ip,
                        str(self._port), qid, self._user, self._password, 30,
                         headers = self._headers)
                    for item in result:
                        res.append(item)
        except Exception as e:
            print str(e) 
        finally:
            return res        

    def post_query(self, table, start_time=None, end_time=None,
                   select_fields=None, where_clause='',
                   sort_fields=None, sort=None, limit=None,
                   filter=None, sync=True,dir=None, session_type=None,
                   is_service_instance=None):
        res = None
        try:
            flows_url = OpServerUtils.opserver_query_url(
                self._ip, str(self._port))
            print flows_url
            query_dict = OpServerUtils.get_query_dict(
                table, start_time=start_time, end_time=end_time,
                select_fields=select_fields,
                where_clause=where_clause,
                sort_fields=sort_fields, sort=sort, limit=limit, filter=filter,
                dir=dir,
                is_service_instance=is_service_instance,
                session_type=session_type)

            print json.dumps(query_dict)
            res = []
            resp = OpServerUtils.post_url_http(
                flows_url, json.dumps(query_dict), self._user, self._password,
                sync, headers=self._headers)
            if sync:
                if resp is not None:
                    res = json.loads(resp)
                    res = res['value']
            else:
                if resp is not None:
                    resp = json.loads(resp)
                    qid = resp['href'].rsplit('/', 1)[1]
                    result = OpServerUtils.get_query_result(
                        self._ip, str(self._port), qid, self._user,
                        self._password, 30, headers=self._headers)
                    for item in result:
                        res.append(item)
        except Exception as e:
            print str(e)
        finally:
            return res

if __name__ == '__main__':
    vns = VerificationOpsSrv(socket.getfqdn("127.0.0.1"))

    vn = vns.get_ops_vn(vn='abc-corp:vn02')

    print "%%% Verify VN Cfg %%%"

    print vn.get_attr('Config', 'attached_policies', 'abc-default-policy')
    '''
    [{u'vnp_major': u'10', u'vnp_name': u'abc-default-policy',
      u'vnp_minor': u'50'}]
    '''

    print vn.get_attr('Config', 'connected_networks')
    '''
    [u'abc-corp:vn04']
    '''

    print vn.get_attr('Config', 'total_interfaces')
    '''
    10
    '''

    print vn.get_attr('Config', 'total_acl_rules')
    '''
    60
    '''

    print "%%% Verify VN Agt %%%"

    print vn.get_attr('Agent', 'total_acl_rules')
    '''
    55
    '''

    print vn.get_attr('Agent', 'in_tpkts')
    '''
    240
    '''

    print vn.get_attr('Agent', 'in_stats', 'abc-corp:map-reduce-02')
    '''
    [{u'bytes': u'7200', u'other_vn': u'abc-corp:map-reduce-02',
      u'tpkts': u'60'}]
    '''

    vm = vns.get_ops_vm(vm='abc-corp:vm-web-fe01')

    print "%%% Verify VM Cfg %%%"

    print vm.get_attr('Config', 'vrouter')
    '''
    rack01-host04
    '''

    print vm.get_attr('Config', 'attached_groups')
    '''
    [u'abc-grp01']
    '''

    print vm.get_attr('Config', 'interface_list', 'abc-corp:vn-fe')
    '''
    [{u'virtual_network': u'abc-corp:vn-fe', u'ip_address': u'10.1.1.2',
      u'floating_ips': [u'67.1.1.2', u'67.1.1.3']}]
    '''

    print "%%% Verify VM Agt %%%"

    print vm.get_attr('Agent', 'vrouter')
    '''
    rack01-host04
    '''

    print vm.get_attr('Agent', 'attached_groups')
    '''
    [u'abc-grp01']
    '''

    print vm.get_attr('Agent', 'interface_list')
    '''
    [{u'in_bytes': u'1000', u'out_bytes': u'10000',
      u'floating_ips': [u'67.1.1.2', u'67.1.1.3'],
      u'out_pkts': u'20', u'virtual_network': u'abc-corp:vn-fe',
      u'in_pkts': u'5', u'ip_address': u'10.1.1.2'}]
    '''

    col = vns.get_ops_collector()

    print col.get_attr('Analytics', 'generator_infos')
    '''
    [{u'gen_attr': {u'http_port': u'8089', u'in_clear': u'false',
                    u'pid': u'57160', u'connects': u'1', u'clears': u'1',
                    u'resets': u'0'},
      u'source': u'sa-nc-mfg-30.static.jnpr.net',
      u'msgtype_stats': {u'SandeshStats':
                             [{u'bytes': u'1363005',
                               u'messages': u'431',
                               u'message_type': u'CollectorInfo'}]},
                         u'module_id': u'Collector'},
     {u'gen_attr': {u'http_port': u'0', u'in_clear': u'false',
                    u'pid': u'0', u'connects': u'1', u'clears': u'0',
                    u'resets': u'0'},
      u'source': u'sa-nc-mfg-30.static.jnpr.net', u'msgtype_stats': {},
      u'module_id': u'OpServer'},
     {u'gen_attr': {u'http_port': u'8091', u'in_clear': u'false',
                    u'pid': u'57200', u'connects': u'2', u'clears': u'2',
                    u'resets': u'1'},
      u'source': u'sa-nc-mfg-30.static.jnpr.net',
      u'msgtype_stats': {u'SandeshStats': [{u'bytes': u'16771',
                                            u'messages': u'66',
                                            u'message_type': u'QELog'},
                                           {u'bytes': u'12912',
                                            u'messages': u'32',
                                            u'message_type': u'QEQueryLog'}]},
      u'module_id': u'QueryEngine'}]
    '''

    print col.get_attr('Analytics', 'generator_infos',
                       [('module_id', 'OpServer'),
                        ('source', "sa-nc-mfg-30.static.jnpr.net")])
    '''
    [{u'gen_attr': {u'http_port': u'0', u'in_clear': u'false', u'pid': u'0',
                    u'connects': u'1', u'clears': u'0', u'resets': u'0'},
      u'source': u'sa-nc-mfg-30.static.jnpr.net', u'msgtype_stats': {},
      u'module_id': u'OpServer'}]
    '''
