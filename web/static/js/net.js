// WebSocket 客户端封装：与页面同源（网关在同一端口上把 WS 桥到游戏服务器），
// 所以地址不用配置，直接按 location 拼。按消息 type 分发监听者。
(function (global) {
    'use strict';

    function Net() {
        this.ws = null;
        this.handlers = {}; // type → [fn]
        this.connected = false;
    }

    Net.prototype.on = function (type, fn) {
        (this.handlers[type] = this.handlers[type] || []).push(fn);
        return this;
    };

    Net.prototype.emit = function (type, data) {
        var list = this.handlers[type];
        if (!list) return;
        for (var i = 0; i < list.length; i++) list[i](data);
    };

    Net.prototype.connect = function () {
        var self = this;
        var proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
        this.ws = new WebSocket(proto + location.host + '/');
        this.ws.onopen = function () {
            self.connected = true;
            self.emit('__open');
        };
        this.ws.onmessage = function (ev) {
            var msg;
            try {
                msg = JSON.parse(ev.data);
            } catch (e) {
                console.error('坏消息（不是 JSON）：', ev.data);
                return;
            }
            if (msg && msg.type) self.emit(msg.type, msg);
            self.emit('__any', msg);
        };
        this.ws.onclose = function () {
            self.connected = false;
            self.emit('__close');
        };
        this.ws.onerror = function () {
            self.emit('__error');
        };
        return this;
    };

    Net.prototype.send = function (obj) {
        if (!this.ws || this.ws.readyState !== 1) return false;
        this.ws.send(JSON.stringify(obj));
        return true;
    };

    global.FGNet = Net;
})(window);
