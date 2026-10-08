<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { withBase } from 'vitepress'

interface Slide {
  key: string
  title: string
  desc: string
}

const props = withDefaults(
  defineProps<{
    slides: Slide[]
    /** 自动轮播间隔（毫秒），0 表示不自动播放 */
    interval?: number
    /** 无障碍区域名（同页多实例时各自指明，如「桌面端界面预览」） */
    label?: string
  }>(),
  { interval: 5000, label: '界面预览' }
)

/** 截图主题：亮 / 暗各一套（screenshots/<key>-light|dark.png） */
const mode = ref<'light' | 'dark'>('light')
const current = ref(0)
const hovering = ref(false)
const reducedMotion = ref(false)
let timer: ReturnType<typeof setInterval> | undefined

const total = computed(() => props.slides.length)
const active = computed(() => props.slides[current.value] ?? props.slides[0])

function src(key: string) {
  return withBase(`/screenshots/${key}-${mode.value}.png`)
}

function go(i: number) {
  current.value = (i + total.value) % total.value
}

function next() {
  go(current.value + 1)
}

function prev() {
  go(current.value - 1)
}

function toggleMode() {
  mode.value = mode.value === 'light' ? 'dark' : 'light'
}

function onKey(e: KeyboardEvent) {
  if (e.key === 'ArrowRight') {
    e.preventDefault()
    next()
  } else if (e.key === 'ArrowLeft') {
    e.preventDefault()
    prev()
  }
}

let touchX = 0

function onTouchStart(e: TouchEvent) {
  touchX = e.touches[0]?.clientX ?? 0
}

function onTouchEnd(e: TouchEvent) {
  const dx = (e.changedTouches[0]?.clientX ?? 0) - touchX
  if (Math.abs(dx) > 48) (dx < 0 ? next : prev)()
}

function stopTimer() {
  if (timer) {
    clearInterval(timer)
    timer = undefined
  }
}

function startTimer() {
  stopTimer()
  if (!reducedMotion.value && props.interval > 0 && !hovering.value) {
    timer = setInterval(next, props.interval)
  }
}

watch([hovering, reducedMotion], startTimer)

onMounted(() => {
  reducedMotion.value = window.matchMedia('(prefers-reduced-motion: reduce)').matches
  startTimer()
})

onBeforeUnmount(stopTimer)
</script>

<template>
  <div
    class="showcase"
    role="region"
    aria-roledescription="轮播"
    :aria-label="label"
    tabindex="0"
    @mouseenter="hovering = true"
    @mouseleave="hovering = false"
    @focusin="hovering = true"
    @focusout="hovering = false"
    @keydown="onKey"
    @touchstart.passive="onTouchStart"
    @touchend.passive="onTouchEnd"
  >
    <div class="showcase-viewport">
      <div
        class="showcase-track"
        :class="{ 'no-anim': reducedMotion }"
        :style="{ transform: `translateX(-${current * 100}%)` }"
      >
        <div v-for="(s, i) in slides" :key="s.key" class="showcase-slide" :aria-hidden="i !== current">
          <img
            :src="src(s.key)"
            :alt="`${s.title}（${mode === 'light' ? '浅色' : '深色'}）`"
            :loading="i === 0 ? 'eager' : 'lazy'"
            draggable="false"
          />
        </div>
      </div>

      <button class="showcase-arrow left" aria-label="上一张" @click="prev">‹</button>
      <button class="showcase-arrow right" aria-label="下一张" @click="next">›</button>

      <button class="showcase-mode" :aria-label="mode === 'light' ? '切换到深色截图' : '切换到浅色截图'" @click="toggleMode">
        {{ mode === 'light' ? '🌙 深色' : '☀️ 浅色' }}
      </button>
    </div>

    <div class="showcase-caption">
      <div class="showcase-text">
        <b>{{ active.title }}</b>
        <span>{{ active.desc }}</span>
      </div>
      <div class="showcase-dots" role="tablist" aria-label="选择截图">
        <button
          v-for="(s, i) in slides"
          :key="s.key"
          class="showcase-dot"
          :class="{ on: i === current }"
          role="tab"
          :aria-selected="i === current"
          :aria-label="s.title"
          @click="go(i)"
        />
      </div>
    </div>
  </div>
</template>

<style scoped>
.showcase {
  position: relative;
  border: 1px solid var(--vp-c-divider);
  border-radius: 12px;
  background: var(--vp-c-bg-soft);
  overflow: hidden;
  outline: none;
}

.showcase:focus-visible {
  border-color: var(--vp-c-brand-1);
  box-shadow: 0 0 0 2px var(--vp-c-brand-soft);
}

.showcase-viewport {
  position: relative;
  overflow: hidden;
  background: var(--vp-c-bg-mute);
}

.showcase-track {
  display: flex;
  transition: transform 0.45s ease;
}

.showcase-track.no-anim {
  transition: none;
}

.showcase-slide {
  flex: 0 0 100%;
  min-width: 0;
}

.showcase-slide img {
  display: block;
  width: 100%;
  height: auto;
  user-select: none;
}

.showcase-arrow {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  width: 38px;
  height: 38px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 8px;
  background: var(--vp-c-bg-elv);
  color: var(--vp-c-text-1);
  font-size: 22px;
  line-height: 1;
  cursor: pointer;
  opacity: 0.85;
  transition: opacity 0.2s, border-color 0.2s;
}

.showcase-arrow:hover {
  opacity: 1;
  border-color: var(--vp-c-brand-1);
  color: var(--vp-c-brand-1);
}

.showcase-arrow.left {
  left: 12px;
}

.showcase-arrow.right {
  right: 12px;
}

.showcase-mode {
  position: absolute;
  top: 12px;
  right: 12px;
  padding: 4px 12px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 8px;
  background: var(--vp-c-bg-elv);
  color: var(--vp-c-text-1);
  font-size: 12.5px;
  cursor: pointer;
  transition: border-color 0.2s, color 0.2s;
}

.showcase-mode:hover {
  border-color: var(--vp-c-brand-1);
  color: var(--vp-c-brand-1);
}

.showcase-caption {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 12px 16px;
  border-top: 1px solid var(--vp-c-divider);
}

.showcase-text {
  display: flex;
  align-items: baseline;
  gap: 10px;
  min-width: 0;
  flex-wrap: wrap;
}

.showcase-text b {
  font-size: 14.5px;
  color: var(--vp-c-text-1);
}

.showcase-text span {
  font-size: 13px;
  color: var(--vp-c-text-2);
}

.showcase-dots {
  display: flex;
  gap: 7px;
  flex: none;
}

.showcase-dot {
  width: 8px;
  height: 8px;
  padding: 0;
  border: none;
  border-radius: 50%;
  background: var(--vp-c-text-3);
  opacity: 0.35;
  cursor: pointer;
  transition: opacity 0.2s, transform 0.2s, background 0.2s;
}

.showcase-dot.on {
  opacity: 1;
  background: var(--vp-c-brand-1);
  transform: scale(1.15);
}

@media (max-width: 640px) {
  .showcase-arrow {
    display: none;
  }

  .showcase-caption {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
