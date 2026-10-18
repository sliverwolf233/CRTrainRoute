/**
 * 地图配置模板
 *
 * 使用方法：
 *   1. 复制本文件为 config.js      cp config.example.js config.js
 *   2. 填入你自己的高德地图 key
 *   3. config.js 已被 .gitignore 忽略，不会提交到仓库
 *
 * 申请地址：https://console.amap.com/dev/key/app
 * 需要申请「Web端 (JS API)」类型的 key，并同时获取对应的安全密钥。
 */
export default {
	keys: {
		amap_js: '',   // 高德 JS API key（Web端）
		amap_sec: ''   // 高德 JS API 安全密钥
	}
}
