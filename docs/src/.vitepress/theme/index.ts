import DefaultTheme from 'vitepress/theme'
import ShowcaseCarousel from './ShowcaseCarousel.vue'
import type { Theme } from 'vitepress'
import './custom.css'

export default {
  extends: DefaultTheme,
  enhanceApp({ app }) {
    app.component('ShowcaseCarousel', ShowcaseCarousel)
  }
} satisfies Theme
