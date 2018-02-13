<?php
# Enable display of all errors
error_reporting(E_ALL); 
ini_set('display_errors', 1);

function getdb() {
  # Method 'trust' avoid to use password. Config in /etc/postgresql/10.6/main/pg_hba.conf
  $db = pg_connect("host=127.0.0.1 user=postgres") or die('connection failed');
  return $db;
}

$db = getdb();
?>

<html>
  <head>
    <title>Test PHP</title>
    <style>
      #conteneur {
        display: flex;
      }
     </style>
     <script>
       function mouseOver(str) {
         var dec = str.split(" ");
	 var wpc = dec[1].substring(1,dec[1].length-1).split(",");
	 var ids = wpc[dec[0]-1].substring(1,wpc[dec[0]-1].length-1).split(";");
	 for(var i = 0; i < ids.length; i++) {
           document.getElementById(ids[i]).style.color = "red";
	 }
       }

       function mouseOut(str) {
         var dec = str.split(" ");
	 var wpc = dec[1].substring(1,dec[1].length-1).split(",");
	 var ids = wpc[dec[0]-1].substring(1,wpc[dec[0]-1].length-1).split(";");
	 for(var i = 0; i < ids.length; i++) {
           document.getElementById(ids[i]).style.color = "black";
	 }
       }
     </script>
  </head>
  <body>
    <div id="conteneur">
      <div id="left-panel">
    <?php
    # PHP CODE FOR LEFT PANEL:
    #  list and print all tables tagged with provenance  
    $req = "
      SELECT relname, 
             a1.attrelid,
             (SELECT count(*) FROM pg_attribute a2 WHERE a2.attrelid=a1.attrelid AND attnum>0)-1 c
      FROM pg_attribute a1 	JOIN pg_type 		ON atttypid=pg_type.oid
                           	JOIN pg_namespace ns1 	ON typnamespace=ns1.oid
                           	JOIN pg_class 		ON attrelid=pg_class.oid
                           	JOIN pg_namespace ns2 	ON relnamespace=ns2.oid
      WHERE typname='provenance_token' 
        AND relkind='r' 
        AND ns1.nspname='provsql' 
        AND ns2.nspname<>'provsql' 
        AND attname='provsql'";
    $r = pg_exec($db , $req); 
    for ($i=0; $i<pg_numrows($r); $i++) {
      $l=pg_fetch_array($r,$i);
      echo "<h1>".$l["relname"]."</h1>";
      echo "<table>";
      # ENTETES: affichage des noms de colonnes en balise <th>
      $reqh = " 
        SELECT attname
        FROM pg_attribute
	WHERE attrelid = ".$l["attrelid"]."
          AND attnum > 0
        ORDER BY attnum";
      echo "<tr>";
      $rh = pg_exec($db, $reqh);
      for ($ih=0; $ih<pg_numrows($rh)-1; $ih++) {
	$lh = pg_fetch_array($rh,$ih);
        echo "<th>".$lh["attname"]."</th>"; 
      }
      echo "</tr>";
      # CONTENU: affichage des données avec en balise <tr>
      $nil = pg_exec($db, "SET search_path to public,provsql");
      $r2 = pg_exec($db, "select * from (select * from ".$l["relname"]." ) t");
      for ($i2=0; $i2<pg_numrows($r2); $i2++) {
        $l2=pg_fetch_array($r2,$i2);
	echo '<tr title="'.$l2[$l["c"]].'">';
        for ($j=0; $j<$l["c"]; $j++) {
           echo '<td id="'.$l["relname"].':'.$l2[$l["c"]].':'.($j+1).'">'.$l2[$j].'</td>';
        }
        echo "</tr>";
      }
      echo "</table>";
    }
    # END PHP CODE FOR LEFT PANEL
    ?>
      </div>
      <div id="right-panel">
        <p> SQL query (without ending semicolon): </p>
        <form method="post" action="index.php">
	  <textarea name="request" rows=2 cols=150><?php
              if($_POST) echo $_POST['request'];
              else echo 'SELECT distinct city from personnel'; 
            ?></textarea>
          <input type="submit" name="button" value=" Send request ">
        </form>
        <?php
          # AFFICHAGE: résultats de la requête et where en surbrillance
          if($_POST) { 
            $nil = pg_exec($db, "SET search_path to public,provsql");
            $requ = "select *, provsql.where_provenance(provsql.provenance()) from (".$_POST['request'].") t";
	    $ru = pg_exec($db, $requ);
	    echo "<table>";
            $l3=pg_fetch_array($ru,0);
	    echo "<tr>";
	    for ($j=0; $j<sizeof($l3)/2-2; $j++) {
	      echo "<th>".array_keys($l3)[2*$j+1]."</th>";
	    }
	    echo "</tr>";

	    for ($i3=0; $i3<pg_numrows($ru); $i3++) {
              $l3=pg_fetch_array($ru,$i3);
	      echo '<tr title="'.$l3[sizeof($l3)/2-1].'">';
	      #echo "<tr>";
              for ($j=0; $j<sizeof($l3)/2-2; $j++) {
	        echo '<td id="'.($j+1).' '.$l3[sizeof($l3)/2-2].'" onmouseover="mouseOver(this.id)" onmouseout="mouseOut(this.id)" >'.$l3[$j].'</td>';
              }
	      echo "</tr>";
            }
	    echo "</table>";
	  }
        ?>
      </div>
    </div>
  </body>
</html>
