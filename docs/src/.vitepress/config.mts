import { defineConfig } from 'vitepress'

const sharedSidebarOrder = [
  'product',
  'prototypes',
  'requirements',
  'roadmap',
  'decisions',
  'report',
  'compliance',
  'authorization',
  'feiq-compat',
  'file-storage',
  'backup-restore',
  'wan-drill',
  'notify',
  'settings',
  'crash-reporting',
  'platform-adapter',
  'delivery-matrix',
  'av-collab'
] as const

const zhLabels: Record<string, string> = {
  product: '产品定位',
  prototypes: '界面原型与预览',
  requirements: '需求与验收',
  roadmap: '四期规划',
  decisions: '决策清单',
  report: '评审报告摘要',
  compliance: '合规与命名一致性',
  authorization: '服务端授权盘点',
  'feiq-compat': '飞秋/IPMSG 兼容',
  'file-storage': '文件存储与生命周期',
  'backup-restore': '备份恢复',
  'wan-drill': '跨地域与断线补传验证',
  notify: '通知与 webhook 接入',
  settings: '客户端设置',
  'crash-reporting': '崩溃采集（Crashpad）',
  'platform-adapter': '国产系统适配验证',
  'delivery-matrix': '交付验收矩阵',
  'av-collab': '音视频与协同文档底座',
  development: '克隆与构建'
}

const enLabels: Record<string, string> = {
  product: 'Product Positioning',
  prototypes: 'UI Prototypes and Preview',
  requirements: 'Requirements and Acceptance',
  roadmap: 'The Four-Phase Plan',
  decisions: 'Decision List',
  report: 'Review Report Summary',
  compliance: 'Compliance and Naming Consistency',
  authorization: 'Server-Side Authorization Inventory',
  'feiq-compat': 'FeiQ / IPMSG Compatibility',
  'file-storage': 'File Storage and Lifecycle',
  'backup-restore': 'Backup and Restore',
  'wan-drill': 'Cross-Region Access and Re-delivery Drills',
  notify: 'Notification Subsystem and Webhook Access',
  settings: 'Client Settings',
  'crash-reporting': 'Crash Reporting (Crashpad)',
  'platform-adapter': 'Domestic-OS Adaptation Verification',
  'delivery-matrix': 'Delivery Acceptance Matrix',
  'av-collab': 'Audio/Video and Collaborative Document Base',
  development: 'Clone and Build'
}

const buildSidebar = (prefix: string, labels: Record<string, string>, group: string, devGroup: string) => [
  {
    text: group,
    items: sharedSidebarOrder.map((slug) => ({ text: labels[slug], link: `${prefix}/guide/${slug}` }))
  },
  {
    text: devGroup,
    items: [{ text: labels.development, link: `${prefix}/guide/development` }]
  }
]

export default defineConfig({
  title: 'Memex',
  lang: 'en-US',
  base: '/memex/',
  cleanUrls: true,
  lastUpdated: true,
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/memex/logo.svg' }],
    ['meta', { name: 'theme-color', content: '#e16531' }]
  ],
  locales: {
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      link: '/zh/',
      description: '内网办公即时通讯系统 · 对标企业微信 · 纯自研 · 物理隔离内网部署',
      themeConfig: {
        nav: [
          { text: '指南', link: '/zh/guide/product', activeMatch: '/zh/guide/' },
          { text: '需求与验收', link: '/zh/guide/requirements' },
          { text: '决策清单', link: '/zh/guide/decisions' },
          { text: '评审报告', link: '/zh/guide/report' }
        ],
        sidebar: buildSidebar('/zh', zhLabels, '指南', '开发'),
        outline: { label: '本页目录', level: [2, 3] },
        docFooter: { prev: '上一页', next: '下一页' },
        lastUpdatedText: '最后更新于',
        returnToTopLabel: '回到顶部',
        sidebarMenuLabel: '菜单',
        darkModeSwitchLabel: '主题',
        lightModeSwitchTitle: '切换到浅色模式',
        darkModeSwitchTitle: '切换到深色模式',
        footer: {
          message: '内网办公即时通讯系统 · 双引擎客户端 · 留痕原则',
          copyright: '对标企业微信 · 纯自研 · 物理隔离内网部署'
        }
      }
    },
    root: {
      label: 'English',
      lang: 'en',
      description: 'Intranet office IM · Enterprise-WeChat-class · Fully in-house · Air-gapped deployment',
      themeConfig: {
        nav: [
          { text: 'Guide', link: '/guide/product', activeMatch: '/guide/' },
          { text: 'Requirements', link: '/guide/requirements' },
          { text: 'Decisions', link: '/guide/decisions' },
          { text: 'Review Report', link: '/guide/report' }
        ],
        sidebar: buildSidebar('', enLabels, 'Guide', 'Development'),
        outline: { label: 'On this page', level: [2, 3] },
        docFooter: { prev: 'Previous', next: 'Next' },
        lastUpdatedText: 'Last updated',
        returnToTopLabel: 'Return to top',
        sidebarMenuLabel: 'Menu',
        darkModeSwitchLabel: 'Appearance',
        lightModeSwitchTitle: 'Switch to light theme',
        darkModeSwitchTitle: 'Switch to dark theme',
        footer: {
          message: 'Intranet office IM · Dual-engine clients · Traceability first',
          copyright: 'Enterprise-WeChat-class · Fully in-house · Air-gapped deployment'
        }
      }
    },
  },
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: 'Memex',
    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/memex' }]
  }
})
