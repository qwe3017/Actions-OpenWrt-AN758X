// SPDX-License-Identifier: Apache-2.0
//
// NAT 类型选择页 —— 网络 → NAT 类型
//
// 菜单声明：root/usr/share/luci/menu.d/luci-app-natmode.json
//   "admin/network/natmode" → action.path "natmode/mode"
//   → 对应 resources/view/natmode/mode.js（本文件）
//
// 三档说明见 /usr/sbin/natmode-apply 顶部注释。
// 保存后通过 fs.exec 调用 natmode-apply apply 真正生效（ACL 已授权）。

'use strict';
'require view';
'require form';
'require fs';
'require ui';

function parseStatus(text) {
	var st = { mode: '?', effective: '?', fullcone: '0',
	           random_rules: '0', srcnat_chains: '0', offload: 'off',
	           fullcone6: '0', fullcone6_opt: '0',
	           fw_mode: 'restricted', module: '?', synced: '0', healed: '0' };
	(text || '').split('\n').forEach(function(line) {
		var kv = line.split('=');
		if (kv.length >= 2)
			st[kv[0].trim()] = kv.slice(1).join('=').trim();
	});
	return st;
}

function modeLabel(m) {
	switch (m) {
		case 'fullcone':   return _('全锥形NAT') + '（NAT1）';
		case 'restricted': return _('受限型NAT') + '（NAT3）';
		case 'symmetric':  return _('全对称型NAT') + '（NAT4）';
		default:           return m;
	}
}

function offloadLabel(v) {
	switch (v) {
		case 'hw':  return _('硬件卸载');
		case 'sw':  return _('软件卸载');
		default:    return _('关闭');
	}
}

// 防火墙页（网络 → 防火墙 → 常规设置）此刻的显示状态。
// 那个「启用 FullCone NAT」复选框读的就是 firewall.@defaults[0].fullcone，
// 与本插件同一个 UCI 键 —— 所以本页改动后防火墙页会同步反映。
//
// 但要注意：防火墙页只能表达「开 / 关 fullcone」两种状态，
// 无法表达 NAT4（随机端口）。故 NAT4 与「受限型」在防火墙页看起来一样
// （都是未勾选），真正的差异只在本页的「随机端口规则」上。
function fwPageLabel(st) {
	if (st.fw_mode === 'fullcone')
		return _('已勾选「启用 FullCone NAT」');
	if (st.effective === 'symmetric')
		return _('未勾选（防火墙页无法表达 NAT4，随机端口仅本页可见）');
	return _('未勾选「启用 FullCone NAT」');
}

function renderStatus(st) {
	var rows = [
		_('当前模式'),        modeLabel(st.effective),
		_('FullCone 开关'),   (st.fullcone === '1' ? _('已启用') : _('已关闭')),
		_('IPv6 FullCone'),   (st.fullcone6 === '1' ? _('已启用') : _('已关闭'))
			+ (st.effective === 'fullcone' ? '' : _('（仅全锥形时生效）')),
		_('防火墙页对应状态'), fwPageLabel(st),
		_('随机端口规则'),    (st.random_rules !== '0'
			? _('已注入 ') + st.random_rules + _(' 条') : _('无')),
		_('srcnat 链'),       st.srcnat_chains + _(' 个'),
		_('路由/NAT 卸载'),   offloadLabel(st.offload),
		_('fullcone 内核模块'), (st.module === 'loaded' ? _('已加载') : _('未加载'))
	];

	var notice = [];  // 蓝色提示
	var warn = [];    // 黄色警告

	// 反向同步提示：natmode-apply status 发现 firewall 侧被改过时，
	// 会把实际生效的模式写回 natmode.main.mode（synced=1）。
	if (st.synced === '1')
		notice.push(E('p', {}, _('已与防火墙同步：检测到你在「网络 → 防火墙 → 常规设置」'
			+ '改动过 FullCone 开关，本页已按实际生效状态更新为 ')
			+ modeLabel(st.effective) + _('。')));

	// 自愈提示：NAT4 的 nft 规则被 fw4 reload 冲掉后，status 会自动补回
	if (st.healed === '1')
		notice.push(E('p', {}, _('已自动修复：NAT4 的随机端口规则此前被防火墙重载清除，现已重新注入。')));

	if (st.module !== 'loaded' && st.effective === 'fullcone')
		warn.push(E('p', {}, _('未检测到 nft_fullcone 模块，全锥形可能不生效。')));

	// NAT4 与卸载互斥 —— 这是「NAT4 设置了却不生效」最常见的原因
	if (st.effective === 'symmetric' && st.offload !== 'off')
		warn.push(E('p', {}, _('NAT4 与路由/NAT 卸载互斥：卸载流量绕过 conntrack，'
			+ '随机端口规则不参与转发，实测仍是 NAT3。'
			+ '请关闭卸载，或勾选下方「应用 NAT4 时自动关闭卸载」后重新保存。')));
	else if (st.effective === 'fullcone' && st.offload !== 'off')
		warn.push(E('p', {}, _('已开启路由/NAT 卸载，卸载流量绕过 conntrack，'
			+ '可能使全锥形行为不稳定。测 NAT 类型时建议临时关闭卸载。')));

	if (st.effective === 'symmetric' && st.srcnat_chains === '0')
		warn.push(E('p', {}, _('未找到 fw4 的 srcnat_<zone> 链：'
			+ 'WAN 区域可能未启用 MASQUERADE，随机端口规则无处可插。')));

	var table = E('table', { 'class': 'table' });
	for (var i = 0; i < rows.length; i += 2) {
		table.appendChild(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, [ rows[i] ]),
			E('td', { 'class': 'td left' }, [ rows[i + 1] || '?' ])
		]));
	}

	var children = [ E('h3', _('当前状态')), table ];
	notice.forEach(function(w) {
		children.push(E('div', { 'class': 'alert-message notice' }, [ w ]));
	});
	warn.forEach(function(w) {
		children.push(E('div', { 'class': 'alert-message warning' }, [ w ]));
	});

	return E('div', { 'class': 'cbi-section' }, children);
}

return view.extend({
	load: function() {
		return L.resolveDefault(fs.exec_direct('/usr/sbin/natmode-apply', ['status']), '');
	},

	render: function(statusText) {
		var st = parseStatus(statusText);

		// =========================================================
		// 必须用 form.Map，不能用 form.JSONMap！
		//
		// form.js 中 CBIJSONMap 的实现：
		//   __init__(data, ...) { this.config='json';
		//                        this.data = new CBIJSONConfig(data); }
		// 它把第一个参数当作「JSON 数据对象」而非文件名，
		// 且 parsechain=['json'] —— 用于 JSON 配置文件（如 luci 的
		// 某些 js 配置），不是 UCI。
		// /etc/config/natmode 是标准 UCI 文件，必须用 form.Map，
		// 否则解析失败、保存也写不回去。
		// =========================================================
		var m = new form.Map('natmode', _('NAT 类型'),
			_('选择路由器对内网出向连接的 NAT 行为。数字越小越宽松，P2P / 游戏 / PT 体验越好。'));

		var s = m.section(form.NamedSection, 'main', 'natmode');
		s.anonymous = false;

		// =========================================================
		// 必须用 form.ListValue + widget='radio'，不能用 form.RadioValue！
		//
		// luci-base 的 form.js 里根本没有 RadioValue 这个类：
		//   可选类只有 Value / DynamicList / ListValue / RichListValue /
		//   RangeSliderValue / Flag / MultiValue / TextValue / DummyValue /
		//   Button / HiddenValue / FileUpload / DirectoryPicker / SectionValue
		// 传 undefined 进去，AbstractSection.option() 做
		//   L.Class.isSubclass(...) 检查失败 → 抛
		//   TypeError: Class must be a descendant of CBIAbstractValue
		//
		// ListValue 支持 widget='select'（默认）或 'radio'，
		// 配合 orientation='vertical' 就是竖排单选按钮。
		// =========================================================
		var o = s.option(form.ListValue, 'mode', _('NAT 类型'));
		o.widget = 'radio';
		o.orientation = 'vertical';
		o.value('fullcone',
			_('全锥形NAT') + '（NAT1）— ' +
			_('最宽松，端点无关映射 + 端点无关过滤。游戏联机、PT 做种、PCDN 最优。'));
		o.value('restricted',
			_('受限型NAT') + '（NAT3）— ' +
			_('系统默认。端点无关映射 + 地址端口相关过滤，日常上网无影响。'));
		o.value('symmetric',
			_('全对称型NAT') + '（NAT4）— ' +
			_('端口完全随机，映射不可预测，打洞基本不可用。仅用于特殊合规场景。'));
		o.default = 'fullcone';

		// IPv6 FullCone 独立开关，默认【不勾选】。
		//
		// form.Flag 对应 UCI 里的 '0'/'1'；未设置时回落到 default。
		// 默认值必须是字符串 '0'，不能是数字 0 ——
		// form.js 的 Flag 用 === '1' 判定，数字会落到 false 分支但
		// 写入时可能出现类型不一致，统一用字符串最稳。
		var o6 = s.option(form.Flag, 'fullcone6',
			_('同时开启 IPv6 FullCone NAT（fullcone6）'),
			_('对应 firewall.@defaults[0].fullcone6。'
			+ 'IPv6 通常有公网前缀、不做 NAT，收益有限；'
			+ '少数环境下反而会导致 IPv6 连接异常，故默认关闭。'
			+ '仅在选择「全锥形NAT（NAT1）」时生效，其余模式下该键会被删除。'));
		o6.default = '0';

		var oc = s.option(form.Flag, 'auto_offload',
			_('应用 NAT4 时自动关闭路由/NAT 卸载'),
			_('NAT4 的随机端口依赖 nft masquerade，而卸载（尤其硬件卸载走 PPE）'
			+ '会把流量绕过 conntrack 直接转发 —— 两者互斥，开着卸载 NAT4 实测仍是 NAT3。'
			+ '勾选后，选择「全对称型NAT」时会自动关闭卸载（代价：吞吐下降）。'));
		oc.default = '1';

		// 保存后真正应用。
		//
		// 顺序（踩过两次坑，这里说明为什么是这三步）：
		//
		//   1) apply     —— 必须【先】执行。
		//      若先做 ui.changes.apply()，它触发的 firewall reload 会回调
		//      init.d/natmode 的 reapply → do_sync。此时 firewall.fullcone
		//      还是旧值（1），do_sync 会把刚选的 symmetric 改回 fullcone，
		//      表现就是「插件里改不了 NAT 类型」。
		//
		//   2) ui.changes.apply() —— 提交 UCI 改动并触发 firewall reload。
		//      fw4 重建 ruleset，会冲掉第 1 步插入的 nft 随机端口规则。
		//
		//   3) reapply   —— 兜底。按 natmode.main.mode 重建 nft 规则。
		//      do_sync 现在只处理无歧义情况，不会误改 mode，所以这一步安全。
		m.handleSaveApply = function(ev) {
			var self = this;
			return self.handleSave(ev).then(function() {
				return fs.exec('/usr/sbin/natmode-apply', ['apply']);
			}).then(function() {
				return ui.changes.apply();
			}).then(function() {
				return fs.exec('/usr/sbin/natmode-apply', ['reapply']);
			}).then(function() {
				ui.addNotification(null,
					E('p', _('NAT 模式已应用，防火墙已重载。')), 'success');
				window.setTimeout(function() { window.location.reload(); }, 1500);
			}).catch(function(e) {
				ui.addNotification(null,
					E('p', _('应用失败：') + (e && e.message ? e.message : e)), 'error');
			});
		};

		// =========================================================
		// m.render() 返回的是 Promise，不是 DOM 节点！
		//
		// form.js: CBIMap.prototype.render()
		//   render() { return this.load().then(this.renderContents.bind(this)); }
		// 而 renderContents() → renderChildren().then(nodes => ...)
		//
		// 若直接 E('div', {}, [ 状态块, m.render() ])，Promise 不会被 E()
		// 解析，页面上就显示 "[object Promise]" —— 表单（单选按钮）
		// 根本没渲染出来，于是无法更改 NAT 类型。
		//
		// 正确做法：等 Promise resolve 拿到节点数组，再组装。
		// =========================================================
		return m.render().then(function(nodes) {
			var kids = [ renderStatus(st) ];
			if (Array.isArray(nodes))
				kids = kids.concat(nodes);
			else if (nodes != null)
				kids.push(nodes);
			return E('div', {}, kids);
		});
	}
});
