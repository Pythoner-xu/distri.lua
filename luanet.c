/*	
    Copyright (C) <2012>  <huangweilook@21cn.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/	
#include "lua.h"  
#include "lauxlib.h"  
#include "lualib.h"  
#include "link_list.h"
#include "KendyNet.h"
#include "Connection.h"
#include <stdio.h>
#include <stdlib.h>
#include "SocketWrapper.h"
#include "Acceptor.h"
#include "Connector.h"
#include "wpacket.h"
#include <signal.h>
#include "SysTime.h"
#include "sync.h"
#include "thread.h"

uint32_t packet_recv = 0;
uint32_t packet_send = 0;
uint32_t send_request = 0;
uint32_t tick = 0;
uint32_t now = 0;
uint32_t s_p = 0;
uint32_t bf_count = 0;
uint32_t clientcount = 0;
uint32_t last_send_tick = 0;
uint32_t recv_count = 0;

static uint16_t recv_sigint = 0;

enum
{
	ENGINE_STOP = -1,
	NEW_CONNECTION = 1,
	DISCONNECT = 2,
	PROCESS_PACKET = 3,
	CONNECT_SUCESSFUL = 4,
	PACKET_SEND_FINISH = 5,
};

void BindFunction(lua_State *lState);  
void RegisterNet(lua_State *L)  
{  
    BindFunction(L);  
}

extern void SendFinish(int32_t bytetransfer,st_io *io);
extern void RecvFinish(int32_t bytetransfer,st_io *io);

struct luaNetEngine
{
	HANDLE engine;
	acceptor_t _acceptor;
	connector_t _connector;
	struct link_list *msgqueue;
	mutex_t  lock;//protect con_events
	struct link_list *con_events;
	thread_t _thread;//run acceptor and connector
	volatile int8_t terminated;
	uint8_t raw;
};

struct luaconnection
{
	struct connection connection;
	struct luaNetEngine *engine;	
};

struct luaNetMsg
{
	list_node next;
	struct luaconnection *connection;
	rpacket_t packet;
	int8_t    msgType;//1,新连接;2,连接断开，3，网络消息
};


void on_process_packet(struct connection *c,rpacket_t r)
{
	struct luaconnection *con = (struct luaconnection *)c;
	struct luaNetMsg *msg = (struct luaNetMsg *)calloc(1,sizeof(*msg));
	msg->msgType = PROCESS_PACKET;
	msg->connection = con;
	msg->packet = r;
	LINK_LIST_PUSH_BACK(con->engine->msgqueue,msg);
}

void _on_disconnect(struct connection *c,int32_t reason)
{
	struct luaconnection *con = (struct luaconnection *)c;
	struct luaNetMsg *msg = (struct luaNetMsg *)calloc(1,sizeof(*msg));
	msg->msgType = DISCONNECT;
	msg->connection = con;
	LINK_LIST_PUSH_BACK(con->engine->msgqueue,msg);
}

void _packet_send_finish(void *con)
{
	struct luaconnection *c  = (struct luaconnection *)con;
	struct luaNetMsg *msg = (struct luaNetMsg *)calloc(1,sizeof(*msg));
	msg->msgType = PACKET_SEND_FINISH;
	msg->connection = c;
	LINK_LIST_PUSH_BACK(c->engine->msgqueue,msg);	
	 
}

struct luaconnection* createluaconnection(uint16_t raw)
{
	struct luaconnection *c = calloc(1,sizeof(*c));
	c->connection.send_list = LINK_LIST_CREATE();
	c->connection._process_packet = on_process_packet;
	c->connection._on_disconnect = _on_disconnect;
	c->connection.next_recv_buf = 0;
	c->connection.next_recv_pos = 0;
	c->connection.unpack_buf = 0;
	c->connection.unpack_pos = 0;
	c->connection.unpack_size = 0;
	c->connection.recv_overlap.c = (struct connection*)c;
	c->connection.send_overlap.c = (struct connection*)c;
	c->connection.raw = raw;
	c->connection.mt = 0;
	c->connection.is_close = 0;
	return c;
}


//connect to remote server,return a connection for future recv and send
int luaConnect(lua_State *L)
{
	struct luaNetEngine *engine = (struct luaNetEngine*)lua_touserdata(L,1);
	const char *ip = lua_tostring(L,2);
	uint16_t port = (uint16_t)lua_tonumber(L,3);
	struct sockaddr_in remote;
	HANDLE sock;
	sock = OpenSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sock < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	remote.sin_family = AF_INET;
	remote.sin_port = htons(port);
	if(inet_pton(INET,ip,&remote.sin_addr) < 0)
	{
		printf("%s\n",strerror(errno));
		lua_pushnil(L);
		return 1;
	}
	if(Connect(sock, (struct sockaddr *)&remote, sizeof(remote)) != 0)
	{
		lua_pushnil(L);
		return 1;
	}
	struct luaconnection *c = createluaconnection(1);
	c->connection.socket = sock;
	c->engine = engine;
	setNonblock(sock);
	Bind2Engine(engine->engine,sock,RecvFinish,SendFinish);
	lua_pushlightuserdata(L,(void*)c);
	return 1;
}

int luaActiveCloseConnection(lua_State *L)
{
	struct luaconnection *c = lua_touserdata(L,1);
	connection_active_close((struct connection*)c);
	return 0;
}

int luaReleaseConnection(lua_State *L)
{
	struct luaconnection *c = lua_touserdata(L,1);
	if(c)
	{
		if(!c->connection.send_overlap.isUsed && !c->connection.recv_overlap.isUsed)
		{
			c->connection.is_close = 1;			
			ReleaseSocketWrapper(c->connection.socket);		
			wpacket_t w;
			while(w = LINK_LIST_POP(wpacket_t,c->connection.send_list))
				wpacket_destroy(&w);
			LINK_LIST_DESTROY(&(c->connection.send_list));
			buffer_release(&(c->connection.unpack_buf));
			buffer_release(&(c->connection.next_recv_buf));
			free(c);
			lua_pushnumber(L,1);
			return 1;
		}
	}
	lua_pushnumber(L,0);
	return 1;
}

static inline void connection_callback(HANDLE s,void *ud,uint16_t type)
{
	struct luaNetEngine *engine = (struct luaNetEngine *)ud;
	struct luaconnection *c = createluaconnection(engine->raw);
	c->connection.socket = s;
	c->engine = engine;
	setNonblock(s);
	struct luaNetMsg *msg = (struct luaNetMsg *)calloc(1,sizeof(*msg));
	msg->msgType = type;
	msg->connection = c;
	msg->packet = NULL;
	mutex_lock(engine->lock);
	LINK_LIST_PUSH_BACK(engine->con_events,msg);
	mutex_unlock(engine->lock);
}

void accept_callback(HANDLE s,void *ud)
{
	connection_callback(s,ud,NEW_CONNECTION);
}

void on_connect_callback(HANDLE s,const char *ip,int32_t port,void*ud)
{
	if(s >= 0)
		connection_callback(s,ud,CONNECT_SUCESSFUL);
	else
		printf("connect to %s:%d failed\n",ip,port);
}

void *_thread_routine(void *arg)
{
	struct luaNetEngine *e = (struct luaNetEngine*)arg;
	while(!e->terminated)
	{
		if(e->_acceptor)
			acceptor_run(e->_acceptor,100);
		connector_run(e->_connector,100);
	}
}

int luaCreateNet(lua_State *L)
{
	const char *ip = lua_tostring(L,1);
	uint16_t port = (uint16_t)lua_tonumber(L,2);
	uint8_t  raw = (uint8_t)lua_tonumber(L,3);
	struct luaNetEngine *e = (struct luaNetEngine *)calloc(1,sizeof(*e));
	e->msgqueue = LINK_LIST_CREATE();
	if(ip)
	{	
		struct listen_arg* args[2];
		args[0] = (struct listen_arg*)calloc(1,sizeof(*args[0]));
		args[0]->ip = ip;
		args[0]->port = port;
		args[0]->accept_callback = &accept_callback;
		args[0]->ud = e;
		args[1] = NULL;
		e->_acceptor = create_acceptor((struct listen_arg**)&args);
		free(args[0]);
	}
	e->_connector = connector_create();
	e->engine = CreateEngine();
	
	e->lock = mutex_create();
	e->con_events = LINK_LIST_CREATE();
	e->terminated = 0;
	e->_thread = create_thread(1);
	e->raw = raw;
	thread_start_run(e->_thread,_thread_routine,e);
	lua_pushlightuserdata(L,e);
	return 1;
}

int luaDestroyNet(lua_State *L)
{
	struct luaNetEngine *e = (struct luaNetEngine *)lua_touserdata(L,1);
	if(e)
	{
		e->terminated = 1;
		thread_join(e->_thread);
		printf("join finish\n");
		if(e->_acceptor)
			destroy_acceptor(&(e->_acceptor));
		connector_destroy(&(e->_connector));
		destroy_thread(&(e->_thread));
		mutex_destroy(&(e->lock));
		struct luaNetMsg *msg;
		while(msg = (struct luaNetMsg *)link_list_pop(e->con_events))
			free(msg);
		while(msg = (struct luaNetMsg *)link_list_pop(e->msgqueue))
			free(msg);			
		LINK_LIST_DESTROY(&(e->msgqueue));
		LINK_LIST_DESTROY(&(e->con_events));
		free(e);
	}
	return 0;
}

static inline void push_msg(lua_State *L,uint16_t type,struct luaconnection *c,rpacket_t rpk)
{
	lua_newtable(L);
	lua_pushnumber(L,type);
	lua_rawseti(L,-2,1);
	if(c)
		lua_pushlightuserdata(L,c);
	else
		lua_pushnil(L);
	lua_rawseti(L,-2,2);
	if(rpk)
		lua_pushlightuserdata(L,rpk);
	else
		lua_pushnil(L);
	lua_rawseti(L,-2,3);
}

int luaPeekMsg(lua_State *L)
{
	
	if(recv_sigint)
	{
		recv_sigint = 0;
		lua_newtable(L);
		push_msg(L,ENGINE_STOP,NULL,NULL);
		lua_rawseti(L,-2,1);		
		return 1;
	}
	
	struct luaNetEngine *engine = (struct luaNetEngine *)lua_touserdata(L,1);
	uint32_t ms = lua_tonumber(L,2);
	
	if(!link_list_is_empty(engine->con_events))
	{
		mutex_lock(engine->lock);
		link_list_swap(engine->msgqueue,engine->con_events);
		mutex_unlock(engine->lock);
	}

	if(link_list_is_empty(engine->msgqueue))
		if(-1 == EngineRun(engine->engine,ms))
				printf("error\n");
	
	int32_t size = link_list_size(engine->msgqueue);
	if(size > 0)
	{
		lua_newtable(L);
		int32_t i = 0;
		int32_t size = link_list_size(engine->msgqueue);
		for( ; i < size; ++i)
		{
			struct luaNetMsg *msg = (struct luaNetMsg *)link_list_pop(engine->msgqueue);
			if(msg->msgType == NEW_CONNECTION || msg->msgType == CONNECT_SUCESSFUL)
			{
				connection_start_recv((struct connection*)msg->connection);
				Bind2Engine(engine->engine,msg->connection->connection.socket,RecvFinish,SendFinish);
			}
		    push_msg(L,msg->msgType,msg->connection,msg->packet);
			lua_rawseti(L,-2,i+1);
			free(msg);
			
		}
	}
	else
		lua_pushnil(L);
	return 1;
	
}

int luaCreateWpacket(lua_State *L)
{
	struct luaconnection *c = (struct luaconnection *)lua_touserdata(L,1);
	rpacket_t r = lua_touserdata(L,2);
	wpacket_t w;
	if(r)
		w = wpacket_create_by_rpacket(NULL,r);
	else
	{
		uint32_t size = lua_tonumber(L,3);
		w = wpacket_create(0,NULL,size,c->connection.raw);	
	}
	lua_pushlightuserdata(L,w);
	return 1;
}

int luaReleaseRpacket(lua_State *L)
{
	rpacket_t r = lua_touserdata(L,1);
	rpacket_destroy(&r);
	return 0;
}

int luaSendPacket(lua_State *L)
{
	struct luaconnection *c = (struct luaconnection *)lua_touserdata(L,1);
	if(c->connection.is_close)
	{
		printf("luaSendPacket is_close:%x\n",(int32_t)c);
		exit(0);
	}
	wpacket_t w = (wpacket_t)lua_touserdata(L,2);
	uint8_t send_finish = (uint8_t)lua_tonumber(L,3);
	if(send_finish)
		lua_pushnumber(L,connection_send(&(c->connection),w,_packet_send_finish));
	else
		lua_pushnumber(L,connection_send(&(c->connection),w,NULL));
	return 1;
}

int luaPacketReadString(lua_State *L)
{
	rpacket_t r = (rpacket_t)lua_touserdata(L,1);
	const char *str = rpacket_read_string(r);
	if(!str)
		lua_pushnil(L);
	else
		lua_pushstring(L,str);
	return 1;
}

int luaPacketWriteString(lua_State *L)
{
	wpacket_t w = (wpacket_t)lua_touserdata(L,1);
	const char *str = lua_tostring(L,2);
	wpacket_write_string(w,str);
	return 0;
}

int luaPacketReadNumber(lua_State *L)
{
	rpacket_t r = (rpacket_t)lua_touserdata(L,1);
	uint32_t val = rpacket_read_uint32(r);
	lua_pushnumber(L,val);
	return 1;
}

int luaPacketWriteNumber(lua_State *L)
{
	wpacket_t w = (wpacket_t)lua_touserdata(L,1);
	uint32_t val = lua_tonumber(L,2);
	wpacket_write_uint32(w,val);
	return 0;
}

int luaAsynConnect(lua_State *L)
{
	struct luaNetEngine *engine = (struct luaNetEngine *)lua_touserdata(L,1);
	const char *ip = lua_tostring(L,2);
	uint16_t port = (uint16_t)lua_tonumber(L,3);
	uint32_t timeout = lua_tonumber(L,4);
	connector_connect(engine->_connector,ip,port,on_connect_callback,(void*)engine,timeout);
	return 0;
}

int luaGetSysTick(lua_State *L)
{
	lua_pushnumber(L,GetSystemMs());
	return 1;
}

static void sig_int(int sig)
{
	recv_sigint = 1;
}

int luaGetHandle(lua_State *L)
{
	struct luaconnection *c = (struct luaconnection *)lua_touserdata(L,1);
	lua_pushnumber(L,c->connection.socket);
	return 1;
}

void BindFunction(lua_State *L)  
{  
    lua_register(L,"Connect",&luaConnect);
    lua_register(L,"ReleaseConnection",&luaReleaseConnection);   
    lua_register(L,"ActiveCloseConnection",&luaActiveCloseConnection);  
    lua_register(L,"CreateNet",&luaCreateNet);  
    lua_register(L,"PeekMsg",&luaPeekMsg);  
    lua_register(L,"CreateWpacket",&luaCreateWpacket);  
    lua_register(L,"ReleaseRpacket",&luaReleaseRpacket);
    lua_register(L,"SendPacket",&luaSendPacket);
    lua_register(L,"PacketReadString",&luaPacketReadString);
    lua_register(L,"PacketWriteString",&luaPacketWriteString);    
    lua_register(L,"AsynConnect",&luaAsynConnect);
    lua_register(L,"GetSysTick",&luaGetSysTick);
    lua_register(L,"PacketReadNumber",&luaPacketReadNumber);
    lua_register(L,"PacketWriteNumber",&luaPacketWriteNumber);
    lua_register(L,"GetHandle",&luaGetHandle);
    lua_register(L,"DestroyNet",&luaDestroyNet);
    
    
    lua_pushnumber(L,ENGINE_STOP);
    lua_setglobal(L,"ENGINE_STOP");
    lua_pushnumber(L,NEW_CONNECTION);
    lua_setglobal(L,"NEW_CONNECTION");
    lua_pushnumber(L,DISCONNECT);
    lua_setglobal(L,"DISCONNECT");
    lua_pushnumber(L,PROCESS_PACKET);
    lua_setglobal(L,"PROCESS_PACKET");
    lua_pushnumber(L,CONNECT_SUCESSFUL);
    lua_setglobal(L,"CONNECT_SUCESSFUL");
    lua_pushnumber(L,PACKET_SEND_FINISH);
    lua_setglobal(L,"PACKET_SEND_FINISH");  
  
    InitNetSystem();
    signal(SIGINT,sig_int);
    signal(SIGPIPE,SIG_IGN);
    printf("load c function finish\n");    
} 
