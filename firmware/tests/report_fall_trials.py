import argparse
import csv
import statistics
from collections import defaultdict
from contextlib import redirect_stdout
from decimal import Decimal, InvalidOperation
from pathlib import Path


FIELDS = "participant trial kind scenario valid impact_s first_alert_s wear_s alerts notes".split()
FALL_KINDS = ("staged_fall", "real_fall")


def seconds(raw):
    try:
        value = Decimal(raw)
    except InvalidOperation as error:
        raise ValueError("时间须为非负有限秒数") from error
    if not value.is_finite() or value < 0:
        raise ValueError("时间须为非负有限秒数")
    return value


def read_trials(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            raise ValueError("请使用试次模板的列名和列顺序")
        seen = set()
        for row in reader:
            try:
                if None in row or any(value is None for value in row.values()):
                    raise ValueError("列数与模板不一致")
                participant = row["participant"].strip()
                trial = row["trial"].strip()
                scenario = row["scenario"].strip()
                if not (participant and trial and scenario):
                    raise ValueError("请填写参加者、试次编号和场景")
                key = (participant, trial)
                if key in seen:
                    raise ValueError("同一参加者的试次编号重复")
                seen.add(key)

                kind, valid = row["kind"].strip(), row["valid"].strip()
                if kind not in (*FALL_KINDS, "adl") or valid not in ("0", "1"):
                    raise ValueError("kind 须为 staged_fall、real_fall 或 adl，valid 须为 0 或 1")
                if valid == "0":
                    yield None
                    continue

                if kind == "adl":
                    duration = seconds(row["wear_s"])
                    alarms = int(row["alerts"])
                    if duration == 0 or alarms < 0:
                        raise ValueError("日常活动的佩戴时间须大于零，报警次数须为非负整数")
                    yield participant, kind, scenario, (duration, alarms)
                    continue

                impact = seconds(row["impact_s"])
                alert = row["first_alert_s"].strip()
                delay = seconds(alert) - impact if alert else None
                if delay is not None and delay < 0:
                    raise ValueError("第一条跌倒提醒须发生在触地后，触地前的提醒另记")
                yield participant, kind, scenario, delay
            except ValueError as error:
                raise ValueError(f"CSV 第 {reader.line_num} 行　{error}") from error


def print_recall(label, delays, deadline):
    hits = [delay for delay in delays if delay is not None and delay <= deadline]
    late = sum(delay is not None and delay > deadline for delay in delays)
    print(f"{label}　{len(hits)}/{len(delays)}（{len(hits) / len(delays):.1%}），"
          f"漏检 {len(delays) - len(hits)} 次（其中迟报 {late} 次）")
    if hits:
        print(f"  命中延迟中位数 {statistics.median(hits):.2f} 秒，"
              f"范围 {min(hits):.2f}–{max(hits):.2f} 秒")


def print_report(rows, deadline):
    falls = defaultdict(list)
    adl = defaultdict(list)
    for record in rows:
        if record is None:
            continue
        participant, kind, scenario, value = record
        if kind == "adl":
            adl[scenario].append(value)
        else:
            falls[kind].append((participant, scenario, value))

    print(f"命中窗口为触地后 {deadline:g} 秒。无效试次 {rows.count(None)} 条。")
    for kind, label in (("staged_fall", "受控跌倒"), ("real_fall", "自然发生的跌倒")):
        trials = falls[kind]
        if not trials:
            print(f"{label}　暂无有效试次")
            continue
        print_recall(label, [delay for _, _, delay in trials], deadline)
        for column, group_label in ((1, "场景"), (0, "参加者")):
            groups = defaultdict(list)
            for trial in trials:
                groups[trial[column]].append(trial[2])
            for value, delays in sorted(groups.items()):
                print_recall(f"  {group_label} {value}", delays, deadline)

    if not adl:
        print("日常活动　暂无有效观察时长")
        return
    intervals = [interval for group in adl.values() for interval in group]
    for label, group in [("日常活动合计", intervals), *sorted(adl.items())]:
        hours = sum(duration for duration, _ in group) / 3600
        alarms = sum(count for _, count in group)
        print(f"{label}　{hours:.3f} 小时，{alarms} 次误报，{alarms / hours:.2f} 次/小时")


def main():
    parser = argparse.ArgumentParser(description="汇总黄山派跌倒试次、提醒延迟和日常误报")
    parser.add_argument("csv", type=Path, help="按模板填写的观察记录")
    parser.add_argument("--output", type=Path, help="汇总文本的保存位置")
    parser.add_argument("--deadline", type=seconds, default=Decimal(8), help="触地后判为命中的秒数，默认 8")
    args = parser.parse_args()
    if not args.deadline:
        parser.error("--deadline 须大于零")
    if args.output and args.output.resolve() == args.csv.resolve():
        parser.error("试次记录和汇总文本须使用不同的文件")

    try:
        rows = list(read_trials(args.csv))
        if args.output:
            with args.output.open("w", encoding="utf-8") as stream, redirect_stdout(stream):
                print_report(rows, args.deadline)
            print(args.output.resolve())
        else:
            print_report(rows, args.deadline)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
