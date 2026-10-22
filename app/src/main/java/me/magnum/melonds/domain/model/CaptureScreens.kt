package me.magnum.melonds.domain.model

/**
 * Which DS screens are kept in screenshots and recordings. Captured frames are 256x384 with the top screen above the bottom one.
 */
enum class CaptureScreens(val sourceY: Int, val sourceHeight: Int) {
    BOTH(0, 384),
    TOP(0, 192),
    BOTTOM(192, 192),
}
