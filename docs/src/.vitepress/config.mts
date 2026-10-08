import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'Memex',
  description: '内网办公即时通讯系统 · 对标企业微信 · 纯自研 · 物理隔离内网部署',
  base: '/memex/',
  cleanUrls: true,
  lastUpdated: true,
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/memex/logo.svg' }],
    ['meta', { name: 'theme-color', content: '#e16531' }]
  ],
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: 'Memex',
    nav: [
      { text: '指南', link: '/guide/product', activeMatch: '/guide/' },
      { text: '需求与验收', link: '/guide/requirements' },
      { text: '决策清单', link: '/guide/decisions' },
      { text: '评审报告', link: '/guide/report' }
    ],
    sidebar: [
      {
        text: '指南',
        items: [
          { text: '产品定位', link: '/guide/product' },
          { text: '界面原型与预览', link: '/guide/prototypes' },
          { text: '需求与验收', link: '/guide/requirements' },
          { text: '四期规划', link: '/guide/roadmap' },
          { text: '决策清单', link: '/guide/decisions' },
          { text: '评审报告摘要', link: '/guide/report' },
          { text: '合规与命名一致性', link: '/guide/compliance' },
          { text: '备份恢复', link: '/guide/backup-restore' },
          { text: '跨地域与断线补传验证', link: '/guide/wan-drill' },
          { text: '通知与 webhook 接入', link: '/guide/notify' },
          { text: '客户端设置', link: '/guide/settings' },
          { text: '崩溃采集（Crashpad）', link: '/guide/crash-reporting' },
          { text: '国产系统适配验证', link: '/guide/platform-adapter' },
          { text: '音视频与协同文档底座', link: '/guide/av-collab' }
        ]
      },
      {
        text: '开发',
        items: [
          { text: '克隆与构建', link: '/guide/development' }
        ]
      }
    ],
    outline: { label: '本页目录', level: [2, 3] },
    docFooter: { prev: '上一页', next: '下一页' },
    lastUpdatedText: '最后更新于',
    returnToTopLabel: '回到顶部',
    sidebarMenuLabel: '菜单',
    darkModeSwitchLabel: '主题',
    lightModeSwitchTitle: '切换到浅色模式',
    darkModeSwitchTitle: '切换到深色模式',
    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/memex' }],
    footer: {
      message: '内网办公即时通讯系统 · 双引擎客户端 · 留痕原则',
      copyright: '对标企业微信 · 纯自研 · 物理隔离内网部署'
    }
  }
})
