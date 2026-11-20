# Tables

Tables contain rows, which contain cells. Each `TableCell` has one stable
node ID, a header/data flag, and `col_span` and `row_span` counts (both 1 by
default). A cell with a span remains a single piece of content.

`build_table_grid` derives occupied slots and cell positions from those
counts. Layout measures the cells at their combined column widths, determines
row heights, and draws borders only at boundaries between cells. A spanning
header's fill covers its whole rectangle. A complete row of unspanned cells,
when available, supplies the column weights and resize handles. Tables
without spans retain their existing layout path.

In HTML, `colspan` and `rowspan` on `td` and `th` map to these counts.
Invalid values become 1; the HTML limits are 1000 columns and 65534 rows.
HTML `rowspan="0"` extends to the end of its `thead`, `tbody`, `tfoot`, or
implicit row group and is resolved to a count on import. A positive row span
is also limited to its HTML row group. Row groups themselves are not stored.

The compact pipe-table syntax represents unspanned cells. Use fenced rows
and cells for spans:

```text
:::table
:::row
:::th colspan=2
Heading over both columns
:::
:::
:::row
:::th rowspan=2
Shared heading
:::
:::cell
First value
:::
:::
:::row
:::cell
Second value
:::
:::
:::
```

The equivalent S-expression attributes are `:colspan 2` and `:rowspan 2`
on `th` or `td`. Export uses fenced rows when a table has spans, so the
counts survive a Markdown or S-expression round trip. See
`examples/tour.md` for a rendered example.

Pressing Enter in a table cell inserts a row after that cell's row. A cell
whose row span crosses the insertion point grows to cover the new row;
new cells are created only in the uncovered columns.

## Width

A table in the text flow is as wide as its content, not the whole column.
Column widths follow a simplified CSS automatic table layout
(`distribute_table_widths`): every column gets its widest content
(max-content) when the sum fits; otherwise each column gets its narrowest
(the longest word, min-content) plus a share of the remaining room in
proportion to how much more it would like. A table only reaches the full
column when its content asks for it.

`width=100%` (Markdown fence), `:width "100%"` (S-expression) or HTML
`width="100%"` / `style="width:100%"` stretch the table to that share of the
column; `full` means 100%. Lengths (`22em`, `300px`) and `auto` keep the
content width. Dragging a column handle turns the shown widths into
weights first (`seed_table_weights`), so a drag never makes the table jump.

A narrower table sits at the start of the column. `align=center` /
`align=right` on the table place it; without that the `table` style role's
`align` (`start`, `center`, `end`) decides. HTML `align="center"` and
`margin: auto` centre it.

## Alignment

Each cell has a horizontal `align` and a vertical `valign`. They resolve
from the most specific to the most general:

- horizontal: the cell, its row, then the table's column spec `cols`;
  default left;
- vertical: the cell, its row, the table; default **top** for data cells
  and **bottom** for header cells, so column labels sit on the rule below
  them (as in LaTeX / booktabs rather than HTML's middle).

Horizontal values: `left`, `center`, `right`, `decimal`. Vertical values:
`top`, `middle`, `bottom`, `baseline`. CSS, HTML and XSL-FO spellings are
accepted on input (`start` / `end`, `before` / `after`, `char`, `centre`).

`cols` is a LaTeX-style column spec, one letter per column: `l`, `c`, `r`,
`d` (`|`, `p{…}` and `@{…}` are skipped; `S` reads as `d`). Words also work:
`cols="left right decimal"`.

**Decimal** cells of a column line up at their decimal point (the first
`.`; a number without one aligns after its last digit), the whole group
set flush right in the column. A header cell over a decimal column (or a
cell spanning several columns) is centred over the numbers. A right-aligned
column whose body cells all read as numbers (`1,234.5`, `−3 %`, `$12`,
empty cells allowed) becomes a decimal column on its own, so GFM `---:`
columns of figures line up without extra markup.

## Head and foot rows

A row can belong to the head or the foot of the table (`part=head` /
`part=foot`, HTML `thead` / `tfoot`). Without marked rows, the leading rows
made only of header cells form the head when body rows follow (the first
row of a GFM table). The rule under the head and the rule above the foot
are drawn heavier, like booktabs' `\midrule`.

## Markup

Markdown pipe tables take the alignment colons of the separator row
(`:--`, `:-:`, `--:`) as the table's `cols`. Fenced tables carry the
attributes directly:

```text
:::table cols=lrd width=100% valign=middle align=center
:::row part=head
:::th
Item
:::
…
:::
:::row
:::cell align=right valign=bottom
…
```

S-expressions use the same keys: `(table :cols "lrd" :width "100%" …)`,
`(tr :part head …)`, `(td :align right :valign bottom …)`. XML writes
`align`, `valign` and `cols` attributes. Export picks the compact pipe
table when only column alignment is set and fenced rows when rows or cells
carry their own alignment or a head / foot part.

HTML import maps `align`, `valign`, `char`, CSS `text-align` /
`vertical-align` (on cells, rows and row groups), `<col align>` /
`<col span>`, `thead` / `tfoot`, the table's percent width and centring
(`align=center`, `margin: auto`).
