#!/usr/bin/env python3
"""模拟固定手柄组合的互斥状态机，防止普通/LB/BACK/BACK+LB 交叉污染。"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto

UP, DOWN, LEFT, RIGHT = 0x1, 0x2, 0x4, 0x8
BACK, LB = 0x20, 0x100
DIRECTIONS = (LEFT, RIGHT, UP, DOWN)


class Mode(Enum):
    """表示一个方向键在完整按住周期内锁定的互斥输入模式。"""

    NONE = auto()
    PLAIN = auto()
    LB = auto()
    BACK = auto()
    BACK_LB = auto()


@dataclass
class Model:
    """模拟 C++ 方向会话状态机并输出插件动作及媒体屏蔽结果。"""

    previous: int = 0
    modes: dict[int, Mode] = field(default_factory=lambda: {d: Mode.NONE for d in DIRECTIONS})

    def step(self, buttons: int) -> tuple[list[str], bool]:
        """推进一次采样，返回插件动作和是否应屏蔽游戏媒体命令。"""
        previous_directions = self.previous & 0xF
        current_directions = buttons & 0xF
        rising = current_directions & ~previous_directions
        released = previous_directions & ~current_directions
        back = bool(buttons & BACK)
        lb = bool(buttons & LB)
        actions: list[str] = []

        for direction in DIRECTIONS:
            if released & direction:
                self.modes[direction] = Mode.NONE
            if not (rising & direction):
                continue
            if back and lb:
                self.modes[direction] = Mode.BACK_LB
            elif back:
                self.modes[direction] = Mode.BACK
            elif lb:
                self.modes[direction] = Mode.LB
            else:
                self.modes[direction] = Mode.PLAIN

        # C++ 与这里都规定：同一采样中的 LB 斜向输入最多执行一个动作，
        # 优先级固定为左、右、下、上。BACK 布局方向可分别保持自己的会话。
        for direction in (LEFT, RIGHT, DOWN, UP):
            if not (rising & direction) or self.modes[direction] is not Mode.LB:
                continue
            actions.append({LEFT: "previous_album", RIGHT: "next_album",
                            DOWN: "cycle_mode", UP: "show_status"}[direction])
            break
        for direction in DIRECTIONS:
            if not (rising & direction):
                continue
            mode = self.modes[direction]
            if mode is Mode.BACK:
                actions.append(f"move:{direction}")
            elif mode is Mode.BACK_LB:
                actions.append(f"layout_value:{direction}")

        captured = any((current_directions & d) and self.modes[d] in
                       (Mode.LB, Mode.BACK, Mode.BACK_LB) for d in DIRECTIONS)
        raw_combo = bool(current_directions and (back or lb))
        self.previous = buttons
        return actions, captured or raw_combo


def require(condition: bool, message: str) -> None:
    """断言一个行为矩阵条件并打印统一结果。"""
    if not condition:
        raise AssertionError(message)
    print(f"[通过] {message}")


def main() -> int:
    """执行普通、LB、BACK 与 BACK+LB 的按键顺序回归。"""
    model = Model()
    actions, blocked = model.step(DOWN)
    require(actions == [] and not blocked, "单独方向键只交给游戏，不触发插件动作")
    actions, blocked = model.step(DOWN | LB)
    require(actions == [] and blocked, "先按方向再补 LB 不补触发插件动作，但立即屏蔽媒体污染")
    model.step(0)

    model.step(LB)
    actions, blocked = model.step(LB | DOWN)
    require(actions == ["cycle_mode"] and blocked, "LB 先按、方向后按只切换循环模式并屏蔽游戏媒体")
    actions, blocked = model.step(LB | DOWN | BACK)
    require(actions == [] and blocked, "LB 方向会话中途补 BACK 不切换到布局动作")
    model.step(0)

    model.step(BACK)
    actions, blocked = model.step(BACK | UP)
    require(actions == [f"move:{UP}"] and blocked, "BACK+上只移动通知并屏蔽播放/恢复")
    actions, blocked = model.step(BACK | UP | LB)
    require(actions == [] and blocked, "BACK 方向会话中途补 LB 不切换缩放模式")
    model.step(0)

    model.step(BACK | LB)
    actions, blocked = model.step(BACK | LB | DOWN)
    require(actions == [f"layout_value:{DOWN}"] and blocked, "BACK+LB+下只缩放通知并屏蔽暂停")
    actions, blocked = model.step(BACK | DOWN)
    require(actions == [] and blocked, "BACK+LB 会话中途松 LB 不退化为移动模式")
    model.step(0)

    model.step(LB)
    actions, blocked = model.step(LB | LEFT | RIGHT)
    require(actions == ["previous_album"] and blocked, "LB 斜向同帧只执行固定优先级的一个动作且媒体仍被屏蔽")

    print("\n严格输入互斥行为矩阵全部通过。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
